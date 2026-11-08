/**
 * Modified version of bzcat.c part of toybox commit 7bf68329eb3b
 * by Rob Landley released under SPDX-0BSD license.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <core/VectorView.hpp>
#include <filereader/BitReader.hpp>
#include <huffman/HuffmanCodingShortBitsCached.hpp>
#include <libsais.h>


/**
 * @see https://github.com/dsnet/compress/blob/master/doc/bzip2-format.pdf
 * @verbatim
 * Symbol               Expression
 * ------------------------------------------
 * BZipFile             := BZipStream (one or more)
 * └──BZipStream        := StreamHeader StreamBlock* StreamFooter
 *    ├──StreamHeader   := HeaderMagic Version Level                          -> readBzip2Header
 *    ├──StreamBlock    := BlockHeader BlockTrees BlockData(Huffman encoded)
 *    │ ├──BlockHeader  := BlockMagic BlockCRC Randomized OrigPtr             -> readBlockHeader
 *    │ ├──BlockTrees   := SymMap NumTrees NumSels Selectors Trees            -> readTrees
 *    | |  ├──SymMap    := MapL1 MapL2{1,16}
 *    │ |  ├──Selectors := Selector{NumSels}
 *    │ |  └──Trees     := (BitLen Delta{NumSyms}){NumTrees}
 *    | └──BlockData    := Huffman-Encoded data
 *    └──StreamFooter   := FooterMagic StreamCRC Padding                      -> Block::eos
 * @endverbatim

 * 1. Run-length encoding (RLE) of initial data
 * 2. Burrows–Wheeler transform (BWT), or block sorting
 * 3. Move-to-front (MTF) transform
 * 4. Run-length encoding (RLE) of MTF result
 * 5. Huffman coding
 * 6. Selection between multiple Huffman tables
 * 7. Unary base-1 encoding of Huffman table selection
 * 8. Delta encoding (Δ) of Huffman-code bit lengths
 * 9. Sparse bit array showing which symbols are used
 */
namespace bzip2
{
using CRC32LookupTable = std::array<uint32_t, 256>;


[[nodiscard]] constexpr CRC32LookupTable
createCRC32LookupTable() noexcept
{
    constexpr auto littleEndian{ false };
    CRC32LookupTable table{};
    for ( uint32_t i = 0; i < table.size(); ++i ) {
        uint32_t c = littleEndian ? i : i << 24U;
        for ( int j = 0; j < 8; ++j ) {
            if ( littleEndian ) {
                c = ( ( c & 1U ) != 0U ) ? ( c >> 1U ) ^ 0xEDB8'8320U : c >> 1U;
            } else {
                c = ( ( ( c & 0x8000'0000U ) != 0U ) ) ? ( c << 1U ) ^ 0x04C1'1DB7U : ( c << 1U );
            }
        }
        table[i] = c;
    }
    return table;
}


static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;

/* a small lookup table: raw data -> CRC32 value to speed up CRC calculation */
alignas( 64 ) constexpr static CRC32LookupTable CRC32_TABLE = createCRC32LookupTable();


[[nodiscard]] constexpr uint32_t
updateCRC32( uint32_t crc,
             uint8_t  data ) noexcept
{
    //return ( crc >> 8U ) ^ CRC32_TABLE[( crc ^ data ) & 0xFFU];
    return ( crc << 8U ) ^ CRC32_TABLE[( ( crc >> 24U ) ^ data ) & 0xFFU];
}


/**
 * Number of bytes processed per iteration by the bzip2 CRC. bzip2 uses the non-reflected (MSB-first)
 * CRC32 with polynomial 0x04C11DB7, so it cannot use the CRC32 CPU instruction and needs slicing tables.
 * Larger slices are faster in isolation but need more lookup-table cache lines.
 */
#ifndef LIBRAPIDARCHIVE_BZIP2_CRC_SLICES
    #define LIBRAPIDARCHIVE_BZIP2_CRC_SLICES 8
#endif

static constexpr unsigned int BZIP2_CRC_SLICES = LIBRAPIDARCHIVE_BZIP2_CRC_SLICES;
static_assert( ( BZIP2_CRC_SLICES == 1 ) || ( BZIP2_CRC_SLICES == 4 ) || ( BZIP2_CRC_SLICES == 8 ),
               "LIBRAPIDARCHIVE_BZIP2_CRC_SLICES must be 1, 4, or 8." );


/**
 * @param SLICE_SIZE number of bytes the table is used for.
 * @return tables where table[k][byte] is the CRC of @p byte followed by k zero bytes, so that table[0]
 *         is the ordinary byte-wise table.
 */
template<unsigned int SLICE_SIZE>
[[nodiscard]] constexpr std::array<CRC32LookupTable, SLICE_SIZE>
createCRC32SliceTables() noexcept
{
    std::array<CRC32LookupTable, SLICE_SIZE> tables{};
    tables[0] = createCRC32LookupTable();
    for ( unsigned int i = 1; i < SLICE_SIZE; ++i ) {
        for ( unsigned int byte = 0; byte < 256; ++byte ) {
            tables[i][byte] = ( tables[i - 1][byte] << 8U ) ^ tables[0][tables[i - 1][byte] >> 24U];
        }
    }
    return tables;
}


/**
 * Slicing-by-@p SLICE_SIZE update for the non-reflected bzip2 CRC32.
 *
 * For a slice of N data bytes d[0..N-1], the first four bytes are XORed with the four current CRC
 * bytes and looked up in the tables for the remaining N-1..N-4 zero bytes, while the later data
 * bytes only depend on the tables for the smaller remaining shifts. The final @c size % N bytes use
 * the byte-wise table as the tail.
 */
template<unsigned int SLICE_SIZE>
[[nodiscard]] uint32_t
updateCRC32SliceByN( uint32_t       crc,
                     const uint8_t* data,
                     size_t         size ) noexcept
{
    static_assert( ( SLICE_SIZE == 1 ) || ( SLICE_SIZE == 4 ) || ( SLICE_SIZE == 8 ),
                   "SLICE_SIZE must be 1, 4, or 8." );

    if constexpr ( SLICE_SIZE == 1 ) {
        for ( size_t i = 0; i < size; ++i ) {
            crc = updateCRC32( crc, data[i] );
        }
        return crc;
    } else {
        alignas( 64 ) static constexpr auto TABLES = createCRC32SliceTables<SLICE_SIZE>();

        size_t i = 0;
        for ( ; i + SLICE_SIZE <= size; i += SLICE_SIZE ) {
            uint32_t result = 0;
#if defined( __GNUC__ ) || defined( __clang__ )
            #pragma GCC unroll 8
#endif
            for ( unsigned int j = 0; j < SLICE_SIZE; ++j ) {
                const auto byte = data[i + j];
                const auto index = j < 4 ? static_cast<uint8_t>( ( crc >> ( 24U - 8U * j ) ) ^ byte ) : byte;
                result ^= TABLES[SLICE_SIZE - 1 - j][index];
            }
            crc = result;
        }

        for ( ; i < size; ++i ) {
            crc = updateCRC32( crc, data[i] );
        }
        return crc;
    }
}


[[nodiscard]] inline uint32_t
updateCRC32( uint32_t    crc,
             const char* data,
             size_t      size ) noexcept
{
    return updateCRC32SliceByN<BZIP2_CRC_SLICES>(
        crc, reinterpret_cast<const uint8_t*>( data ), size );
}


/* Constants for huffman coding */
static constexpr uint32_t MAX_GROUPS = 6;
static constexpr int GROUP_SIZE = 50;       /* 64 would have been more efficient */
static constexpr int MAX_HUFCODE_BITS = 20; /* Longest huffman code allowed */
static constexpr size_t MAX_SYMBOLS = 258;  /* 256 literals + RUNA + RUNB */
static constexpr uint16_t SYMBOL_RUNA = 0;
static constexpr uint16_t SYMBOL_RUNB = 1;

/* bzip2 levels 1-9 use blocks of at most 100000 to 900000 bytes before the BWT. */
static constexpr size_t MAX_BLOCK_SIZE = 900000;


constexpr auto MAGIC_BITS_BLOCK = 0x314159265359ULL;  /* bcd(pi) */
constexpr auto MAGIC_BITS_EOS = 0x177245385090ULL;  /* bcd(sqrt(pi)) */
constexpr auto MAGIC_BITS_SIZE = 48;
constexpr std::string_view MAGIC_BYTES_BZ2 = "BZh";

using BitReader = rapidgzip::BitReader<true, uint64_t>;


/**
 * bzip2's Move-to-Front (MTF) list for the block data. Eight symbols are packed into one 64-bit word, with the
 * symbol at list position p stored in byte p % 8 of word p / 8. This avoids the variable-length memmove that
 * otherwise dominates the literal path for alphabets with a uniform MTF index distribution such as base64.
 */
using PackedMTF = std::array<uint64_t, 256 / 8>;


[[nodiscard]] constexpr PackedMTF
createPackedMTF() noexcept
{
    PackedMTF result{};
    for ( size_t word = 0; word < result.size(); ++word ) {
        uint64_t value = 0;
        for ( size_t byte = 0; byte < 8; ++byte ) {
            value |= static_cast<uint64_t>( word * 8 + byte ) << ( byte * 8 );
        }
        result[word] = value;
    }
    return result;
}


/**
 * @return the symbol byte that is now at the front of the list.
 *
 * The packed representation stores the list in the same byte order as a plain array, so the first 64 positions
 * can be updated as four 16-byte vectors. For alphabets with a uniform MTF index (base64 after BWT, for example),
 * the scalar word loop exits a data-dependent number of times and therefore mispredicts on almost every literal.
 * The vector path shifts all 64 positions unconditionally and blends with a byte mask, which removes that branch
 * at the cost of some extra work for small indices.
 */
[[nodiscard]] forceinline uint8_t
moveToFront( PackedMTF&   mtf,
             const size_t position ) noexcept
{

    const auto wordIndex = position >> 3;  // equivalent to division by sizeof( PackedMTF::value_type )
    const auto shift = static_cast<uint32_t>( ( position & 7U ) * 8U );
    uint64_t previous = ( mtf[wordIndex] >> shift ) & 0xFFU;

    for ( size_t i = 0; i < wordIndex; ++i ) {
        const auto current = mtf[i];
        mtf[i] = ( current << 8U ) | previous;
        previous = current >> 56U;
    }

    const auto current = mtf[wordIndex];
    /* 0x100 << 56 overflows to zero for unsigned, which would give the correct all-ones mask,
     * but spell it out so that the shift is never out of range. */
    const auto mask = shift == 56U ? ~uint64_t( 0 ) : ( ( uint64_t( 0x100 ) << shift ) - 1U );
    mtf[wordIndex] = ( current & ~mask ) | ( ( ( current << 8U ) | previous ) & mask );
    return static_cast<uint8_t>( mtf.front() );
}


/**
 * @return 1..9 representing the bzip2 block size of 100k to 900k
 */
inline uint8_t
readBzip2Header( BitReader& bitReader )
{
    for ( const auto magicByte : MAGIC_BYTES_BZ2 ) {
        const auto readByte = static_cast<char>( static_cast<uint8_t>( bitReader.read<8>() ) );
        if ( readByte != magicByte ) {
            std::stringstream msg;
            msg << "Input header is not BZip2 magic string 'BZh' (0x"
                << std::hex << int('B') << int('Z') << int('h') << std::dec << "). Mismatch at bit position "
                << bitReader.tell() - 8 << " with " << readByte << " (0x" << std::hex
                /* first cast to unsigned of same length so that the hex representation makes sense. */
                << static_cast<int>( static_cast<uint8_t>( readByte ) )
                << ") should be " << magicByte;
            throw std::domain_error( std::move( msg ).str() );
        }
    }

    // Next byte ASCII '1'-'9', indicates block size in units of 100k of
    // uncompressed data. Allocate intermediate buffer for block.
    const auto i = static_cast<char>( static_cast<uint8_t>( bitReader.read<8>() ) );
    if ( ( i < '1' ) || ( i > '9' ) ) {
        std::stringstream msg;
        msg << "Blocksize must be one of '0' (" << std::hex << static_cast<int>( '0' ) << ") ... '9' ("
            << static_cast<int>( '9' ) << ") but is " << i << " (" << static_cast<int>( i ) << ")";
        throw std::domain_error( std::move( msg ).str() );
    }

    return static_cast<uint8_t>( i - '0' );
}


struct Block  // NOLINT(clang-analyzer-optin.performance.Padding)
{
public:
    /**
     * m ibzip2 && src/tools/ibzip2 -P 1 -f -L offsets -i scorep-pragzip-bgzf-16MiB.tar.bz2
     * @verbatim
     * [BZ2Reader] Time spent:
     * decodeBlock                   : 2.99293s
     * readBlockHeader               : 17.9826s
     *     readSymbolMaps            : 0.000444198s
     *     readSelectors             : 0.0294166s
     *     readTrees                 : 0.0168973s
     *     createHuffmanTable        : 16.8445s
     *     burrowsWheelerPreparation : 1.08934s
     * @endverbatim
     */
    struct Statistics
    {
    public:
        void
        merge( const Statistics& other )
        {
            durations.merge( other.durations );
        }

    public:
        struct Durations
        {
        public:
            void
            merge( const Durations& other )
            {
                readBlockHeader           += other.readBlockHeader;
                decodeBlock               += other.decodeBlock;
                readSymbolMaps            += other.readSymbolMaps;
                readSelectors             += other.readSelectors;
                readTrees                 += other.readTrees;
                createHuffmanTable        += other.createHuffmanTable;
                burrowsWheelerPreparation += other.burrowsWheelerPreparation;
            }

        public:
            double readBlockHeader{ 0 };
            double decodeBlock{ 0 };

            /* Parts of readBlockHeader. */
            double readSymbolMaps{ 0 };
            double readSelectors{ 0 };
            double readTrees{ 0 };
            double createHuffmanTable{ 0 };
            double burrowsWheelerPreparation{ 0 };
        };

        Durations durations;
    };

    //using HuffmanCoding = rapidgzip::HuffmanCodingLinearSearch<uint32_t, uint16_t>;
    //using HuffmanCoding = rapidgzip::HuffmanCodingSymbolsPerLength<uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS,
    //                                                               /* CHECK_OPTIMALITY */ false>;
    /**
     * Some quick benchmarks with m ibzip2 && time src/tools/ibzip2 -v -P 1 -d -o /dev/null -f silesia.tar.bz2:
     *  4-bit: 4.421 4.421 4.482 4.591 4.634
     *  6-bit: 4.462 4.506 4.277 4.279 4.485
     *  8-bit: 4.198 4.188 4.176 4.188 4.297
     * 10-bit: 4.203 4.046 4.174 4.164 4.198
     * 11-bit: 4.117 4.249 4.137 4.076 4.073
     * 12-bit: 3.970 4.101 4.177 4.047 4.030 <-
     * 13-bit: 4.199 4.188 4.144 4.102 4.162
     * 14-bit: 4.134 4.217 4.240 4.108 4.244
     * 16-bit: 4.438 4.415 4.306 4.476 4.577
     * 18-bit: 4.542 4.624 4.562 4.581 4.862
     * Tested on AMD Ryzen 3900X.
     */
    using HuffmanCoding = rapidgzip::HuffmanCodingShortBitsCached<
        uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS, /* LUT size */ 12,
        /* REVERSE_BITS */ false, /* CHECK_OPTIMALITY */ false
    >;
    /* Won't work. It will cause a stack overflow because it contains a std::array of size 2^MAX_HUFCODE_BITS = 1 MiB.
     * And the currently committed version also will not work because it reverses the bits as necessary for gzip.
     * I think this step must be skipped for the bzip2 BitReader. */
    //using HuffmanCoding =
    //    rapidgzip::HuffmanCodingReversedBitsCached<uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS>;

public:
    Block() = default;

    ~Block() = default;

    /** Better don't allow copies because the bitreader would be shared, which might be problematic */
    Block( const Block& ) = delete;

    Block&
    operator=( const Block& ) = delete;

    Block( Block&& ) = default;

    Block&
    operator=( Block&& ) = default;

    explicit
    Block( BitReader& bitReader ) :
        m_bitReader( &bitReader )
    {
        readBlockHeader();
    }

    /**
     * First pass, read block's symbols into bwtData[bwtDataCount].
     *
     * This undoes three types of compression: huffman coding, run length encoding,
     * and move to front encoding.  We have to undo all those to know when we've
     * read enough input.
     *
     * It is not automatically called by the constructor and must be called manually for non-EOS blocks.
     * The interface is like this because ParallelBZ2Reader slows down when calling it automatically
     * because it would not be called on the worker threads but on the main thread!
     */
    void
    readBlockData();

    /**
     * @return True if this is a special enf-of-stream bzip2 block, which contains no data.
     */
    [[nodiscard]] constexpr bool
    eos() const noexcept
    {
        return m_atEndOfStream;
    }

    /**
     * @return True if all data has been read from this block.
     */
    [[nodiscard]] constexpr bool
    eob() const noexcept
    {
        return eos() || !bwdata.hasData();
    }

    [[nodiscard]] constexpr bool
    eof() const noexcept
    {
        return m_atEndOfFile;
    }

    [[nodiscard]] BitReader&
    bitReader()
    {
        if ( m_bitReader != nullptr ) {
            return *m_bitReader;
        }
        throw std::invalid_argument( "Block has not been initialized yet!" );
    }

    /**
     * Currently, the logic is limited and might write up to nMaxBytesToDecode + 255 characters
     * to the output buffer! Currently, the caller has to ensure that the output buffer is large enough.
     */
    [[nodiscard]] size_t
    read( const size_t nMaxBytesToDecode,
          char*        outputBuffer )
    {
        const auto t0 = rapidgzip::now();
        const auto result = bwdata.decodeBlock( nMaxBytesToDecode, outputBuffer );
        statistics.durations.decodeBlock += rapidgzip::duration( t0 );
        return result;
    }

    /**
     * @return The current CRC32 checksum of the decoded data. If all the data of this block has been decoded,
     *         this should match header CRC.
     */
    [[nodiscard]] constexpr uint32_t
    dataCRC() const noexcept
    {
        return bwdata.dataCRC;
    }

    /**
     * @return The CRC32 checksum as stored in the bzip2 block header.
     */
    [[nodiscard]] constexpr uint32_t
    headerCRC() const noexcept
    {
        return bwdata.headerCRC;
    }

private:
    template<uint8_t nBits>
    [[nodiscard]] uint32_t
    getBits()
    {
        return static_cast<uint32_t>( bitReader().read<nBits>() );
    }

    [[nodiscard]] uint32_t
    getBits( uint8_t nBits )
    {
        return static_cast<uint32_t>( bitReader().read( nBits ) );
    }

    void
    readBlockHeader();

    void
    readBlockTrees()
    {
        const auto tReadSymbolMaps = rapidgzip::now();
        readSymbolMaps();
        const auto tReadSelectors = rapidgzip::now();
        readSelectors();
        const auto tReadTrees = rapidgzip::now();
        readTrees();

        statistics.durations.readSymbolMaps += rapidgzip::duration( tReadSymbolMaps, tReadSelectors );
        statistics.durations.readSelectors += rapidgzip::duration( tReadSelectors, tReadTrees );
        statistics.durations.readTrees += rapidgzip::duration( tReadTrees );
    }

    void
    readSymbolMaps();

    void
    readSelectors();

    void
    readTrees();

public:
    struct BurrowsWheelerTransformData
    {
        friend Block;

    public:
        /**
         * Currently, the logic is limited and might write up to nMaxBytesToDecode + 255 characters
         * to the output buffer! Currently, the caller has to ensure that the output buffer is large enough.
         */
        [[nodiscard]] size_t
        decodeBlock( size_t nMaxBytesToDecode,
                     char*  outputBuffer );

        [[nodiscard]] constexpr bool
        hasData() const noexcept
        {
            if ( inverseBWT ) {
                return ( inverseBWT->position < writeCount ) || ( inverseBWT->repeatCount > 0 );
            }
            return ( writeCount > 0 ) || ( symbolRepeatCount > 0 );
        }

    private:
        /** Must only be called after initializing the members from outside. */
        void
        prepare();

    private:
        uint32_t origPtr = 0;
        std::array<uint32_t, 256> byteCount{};

        /* These variables are saved when interrupting decode and are required for resuming */
        uint32_t writePos = 0;
        int writeRun = 0;
        uint32_t writeCount = 0;
        int writeCurrent = 0;

        /* This is for resuming run-length compression. */
        uint8_t symbolToRepeat{ 0 };
        uint8_t symbolRepeatCount{ 0 };

        /* libsais inverts the whole block linearly. Its output is still run-length encoded, so one run may
         * span several decodeBlock calls. The presence of this state selects the libsais path. */
        struct InverseBWTData
        {
            size_t position{ 0 };
            size_t repeatCount{ 0 };
            uint8_t repeatByte{ 0 };
        };

        std::optional<InverseBWTData> inverseBWT;

        uint32_t dataCRC = 0xFFFFFFFFL;  /* CRC of block as calculated by us */
        uint32_t headerCRC = 0;  /* what the block data CRC should be */

        /**
         * Simply allocate the maximum block size so we won't run into problem when block sizes changes
         * (e.g. in pbzip2 file). MAX_BLOCK_SIZE is nothing in today's age anyways.
         * The extra element is required because libsais_unbwt_cyclic uses this buffer as its n + 1 element
         * temporary P array when the inverse BWT is computed.
         */
        std::vector<uint32_t> dbuf = std::vector<uint32_t>( MAX_BLOCK_SIZE + 1, 0 );

        /* BWT input bytes. libsais inverts them in place into the run-length encoded output; the classic
         * chase fallback copies them into dbuf first. */
        std::vector<uint8_t> bwtData = std::vector<uint8_t>( MAX_BLOCK_SIZE, 0 );
    };

public:
    Statistics statistics;

    size_t encodedOffsetInBits = 0;
    size_t encodedSizeInBits = 0;

private:
    uint64_t magicBytes{ 0 };
    bool isRandomized{ false };

    /* First pass decompression data (Huffman and MTF decoding) */

    /**
     * mapping table: if some byte values are never used (encoding things
     * like ascii text), the compression code removes the gaps to have fewer
     * symbols to deal with, and writes a sparse bitfield indicating which
     * values were present.  We make a translation table to convert the symbols
     * back to the corresponding bytes.
     */
    std::array<uint8_t, 256> symbolToByte{};
    /** Used for the selectors, which are decoded before the block data. */
    std::array<uint8_t, 256> mtfSymbol{};
    /** Used for the block data, which has a much larger and less skewed index distribution. */
    PackedMTF mtfPacked{};
    unsigned int symbolCount{ 0 };
    /**
     * Every GROUP_SIZE many symbols we switch huffman coding tables.
     * Each group has a selector, which is an index into the huffman coding table arrays.
     *
     * Read in the group selector array, which is stored as MTF encoded
     * bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
     * to the front of the table, so it has a shorter encoding next time.)
     */
    uint16_t selectorsCount{ 0 };

    std::array<char, 32768> selectors{};  // nSelectors=15 bits
    std::array<HuffmanCoding, MAX_GROUPS> huffmanCodings{};
    uint32_t groupCount = 0;

    /* Second pass decompression data (burrows-wheeler transform) */
    BurrowsWheelerTransformData bwdata;

    BitReader* m_bitReader = nullptr;
    bool m_atEndOfStream = false;
    bool m_atEndOfFile = false;
};


/* Read block header at start of a new compressed data block.  Consists of:
 *
 * 48 bits : Block signature, either pi (data block) or e (EOF block).
 * 32 bits : bw->headerCRC
 * 1  bit  : obsolete feature flag.
 * 24 bits : origPtr (Burrows-wheeler unwind index, only 20 bits ever used)
 * 16 bits : Mapping table index.
 *[16 bits]: symToByte[symTotal] (Mapping table.  For each bit set in mapping
 *           table index above, read another 16 bits of mapping table data.
 *           If corresponding bit is unset, all bits in that mapping table
 *           section are 0.)
 *  3 bits : groupCount (how many huffman tables used to encode, anywhere
 *           from 2 to MAX_GROUPS)
 * variable: hufGroup[groupCount] (MTF encoded huffman table data.)
 */
inline void
Block::readBlockHeader()
{
    const auto tReadBlockHeader = rapidgzip::now();

    encodedOffsetInBits = bitReader().tell();
    encodedSizeInBits = 0;

    magicBytes = ( (uint64_t)getBits<24>() << 24U ) | (uint64_t)getBits<24>();
    bwdata.headerCRC = getBits( 32 );
    m_atEndOfStream = magicBytes == MAGIC_BITS_EOS;
    if ( m_atEndOfStream ) {
        /* read byte padding bits */
        const auto nBitsInByte = static_cast<uint8_t>( bitReader().tell() & 7LLU );
        if ( nBitsInByte > 0 ) {
            bitReader().read( uint8_t( 8 ) - nBitsInByte );
        }

        encodedSizeInBits = bitReader().tell() - encodedOffsetInBits;
        m_atEndOfFile = bitReader().eof();
        return;
    }

    if ( magicBytes != MAGIC_BITS_BLOCK ) {
        std::stringstream msg;
        msg << "[BZip2 block header] invalid compressed magic 0x" << std::hex << magicBytes
            << " at offset " << rapidgzip::formatBits( encodedOffsetInBits );
        throw std::domain_error( std::move( msg ).str() );
    }

    isRandomized = getBits<1>() != 0;
    if ( isRandomized ) {
        throw std::domain_error( "[BZip2 block header] deprecated isRandomized bit is not supported" );
    }

    bwdata.origPtr = getBits<24>();
    if ( bwdata.origPtr > MAX_BLOCK_SIZE ) {
        std::stringstream msg;
        msg << "[BZip2 block header] origPtr " << bwdata.origPtr << " is larger than the maximum block size: "
            << MAX_BLOCK_SIZE;
        throw std::logic_error( std::move( msg ).str() );
    }

    readBlockTrees();
    statistics.durations.readBlockHeader += rapidgzip::duration( tReadBlockHeader );
}


inline void
Block::readSymbolMaps()
{
    /**
     * The Mapping table itself is compressed in two parts:
     * huffmanUsedMap: each bit indicates whether the corresponding range [0...15], [16...31] is present.
     * huffman_used_bitmaps: 0-16 16-bit bitmaps
     * The Huffman map gives 0, 10, 11, 100, 101, ... (8-bit) symbols
     * Instead of storing 2 * 256 bytes ( 0b : A, 10b : B, .. ) for the table, the first part is left out.
     * And for short maps, only the first n-th are actually stored.
     * The second half is also assumed to be ordered, so that we only need to store which symbols are actually
     * present.
     * This however means that the Huffmann table can't be frequency sorted, therefore this is done in a
     * second step / table, the mtfSymbol (move to front) map.
     * This would need 256 bits to store the table in huffman_used_bitmaps.
     * These bits are split in groups of 16 and the presence of each group is encoded in huffmanUsedMap
     * to save even more bytes.
     * @verbatim
     *  10001000 00000000     # huffmanUsedMap (bit map)
     *  ^   ^
     *  |   [64,95]
     *  [0...15]
     *  00000000 00100000     # huffman_used_bitmaps[0]
     *  ^          ^    ^
     *  0          10   15
     *          (newline)
     *  00000100 10001001     # huffman_used_bitmaps[1]
     *  ^    ^   ^   ^  ^
     *  64   69  72  76 95
     *       E   H   L  O
     * @endverbatim
     */
    const uint16_t huffmanUsedMap = getBits<16>();
    /* Can at most grow up to 256 symbols, i.e., MAX_SYMBOLS - 2 (RUNA, RUNB). */
    symbolCount = 0;
    for ( int i = 0; i < 16; i++ ) {
        if ( ( huffmanUsedMap & ( 1U << ( 15U - i ) ) ) != 0 ) {
            const auto bitmap = getBits<16>();
            for ( int j = 0; j < 16; j++ ) {
                if ( ( bitmap & ( 1U << ( 15U - j ) ) ) != 0 ) {
                    symbolToByte[symbolCount++] = ( 16 * i ) + j;
                }
            }
        }
    }
}


inline void
Block::readSelectors()
{
    // How many different huffman coding groups does this block use?
    groupCount = getBits<3>();
    if ( ( groupCount < 2 ) || ( groupCount > MAX_GROUPS ) ) {
        std::stringstream msg;
        msg << "[BZip2 block header] Invalid Huffman coding group count " << groupCount;
        throw std::logic_error( std::move( msg ).str() );
    }

    // nSelectors: Every GROUP_SIZE many symbols we switch huffman coding
    // tables.  Each group has a selector, which is an index into the huffman
    // coding table arrays.
    //
    // Read in the group selector array, which is stored as MTF encoded
    // bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
    // to the front of the table, so it has a shorter encoding next time.)
    selectorsCount = getBits<15>();
    if ( selectorsCount == 0 ) {
        std::stringstream msg;
        msg << "[BZip2 block header] The number of selectors " << selectorsCount << " is invalid";
        throw std::logic_error( std::move( msg ).str() );
    }

    /* The "selectors", referring to the Huffman trees to decode a group of 50 symbols, can only range from 0-5,
     * because MAX_GROUPS = 6, and are encoded as zero-terminated 1-bits, where the number of 1-bits represents
     * the Huffman tree ID. */
    static constexpr std::array<uint8_t, ( 1U << MAX_GROUPS )> BITS_TO_SELECTOR =
        [] () {
            std::array<uint8_t, ( 1U << MAX_GROUPS )> result{};
            uint8_t lowestBitsSet{ 0 };
            for ( uint8_t selector = 0; selector < MAX_GROUPS; ++selector ) {
                const auto paddingBitsCount = MAX_GROUPS - selector;
                const auto maxPaddingBits = static_cast<uint8_t>( 1U << paddingBitsCount );
                const auto highestBitsSet = static_cast<uint8_t>( lowestBitsSet << ( MAX_GROUPS - selector ) );
                for ( uint8_t paddingBits = 0; paddingBits < maxPaddingBits; ++paddingBits ) {
                    result[highestBitsSet | paddingBits] = selector;
                }

                lowestBitsSet <<= 1U;
                lowestBitsSet |= 1U;
            }
        /* A full line of 1-bits is not allowed because it would indicate a selector index larger than MAX_GROUPS. */
            result.back() = MAX_GROUPS;
            return result;
        } ();

    std::iota( mtfSymbol.begin(), mtfSymbol.begin() + groupCount, 0 );
    for ( size_t i = 0; i < selectorsCount; i++ ) {
        const auto j = BITS_TO_SELECTOR.at( m_bitReader->peek<MAX_GROUPS>() );
        if ( j >= groupCount ) {
            std::stringstream msg;
            msg << "[BZip2 block header] Could not find zero termination after " << groupCount << " bits";
            throw std::domain_error( std::move( msg ).str() );
        }
        /* Validate before consuming because a full line of MAX_GROUPS 1-bits has no zero terminator to seek past. */
        m_bitReader->seekAfterPeek( j + 1 );

        // Decode MTF to get the next selector, and move it to the front.
        const auto uc = mtfSymbol[j];
        memmove( mtfSymbol.data() + 1, mtfSymbol.data(), j );
        mtfSymbol[0] = uc;
        selectors[i] = static_cast<char>( uc );
    }
}


/**
 * bzip2 blocks are many times larger than usual gzip blocks. That's why multiple Huffman tree per block are
 * supported and necessary. Similarly to deflate, the trees are stores as code lengths per symbol.
 */
inline void
Block::readTrees()
{
    // Read the huffman coding tables for each group, which code for symTotal
    // literal symbols, plus two run symbols (RUNA, RUNB)
    const auto symCount = symbolCount + 2;
    for ( size_t j = 0; j < groupCount; j++ ) {
        // Read lengths
        std::array<uint8_t, MAX_SYMBOLS> lengths{};
        unsigned int hh = getBits<5>();
        for ( unsigned int symbol = 0; symbol < symCount; symbol++ ) {
            while ( true ) {
                // !hh || hh > MAX_HUFCODE_BITS in one test.
                if ( MAX_HUFCODE_BITS - 1 < hh - 1 ) {
                    std::stringstream msg;
                    msg << "[BZip2 block header] start_huffman_length " << hh
                        << " is larger than " << MAX_HUFCODE_BITS << " or zero\n";
                    throw std::logic_error( std::move( msg ).str() );
                }

                // Stop if first bit is 0, otherwise second bit says whether to
                // increment or decrement.
                if ( getBits<1>() != 0 ) {
                    hh += 1 - ( getBits<1>() << 1U );
                } else {
                    break;
                }
            }
            if ( hh > std::numeric_limits<uint8_t>::max() ) {
                std::stringstream msg;
                msg << "[BZip2 block header] The read code length is unexpectedly large: " << hh;
                throw std::logic_error( std::move( msg ).str() );
            }
            lengths[symbol] = static_cast<uint8_t>( hh );
        }

        const auto error = huffmanCodings[j].initializeFromLengths(
            rapidgzip::VectorView<uint8_t>( lengths.data(), symCount ) );
        if ( error != rapidgzip::Error::NONE ) {
            throw std::domain_error( toString( error ) );
        }
    }
}


/**
 * This undoes the Huffman coding.
 */
inline void
Block::readBlockData()
{
    // We've finished reading and digesting the block.  Now read this
    // block's huffman coded symbols from the file and undo the huffman coding
    // and run length encoding, saving the result into bwtData[bwtDataCount++] = uc

    bwdata.byteCount.fill( 0 );
    static constexpr auto PACKED_MTF_INITIAL = createPackedMTF();
    mtfPacked = PACKED_MTF_INITIAL;

    const auto t0 = rapidgzip::now();
    /** @note The loops inside this for-loop are all too short to be profiled.
     *        The overhead becomes disastrously large! It takes 190s to decode instead of 20s. */
    // Loop through compressed symbols.  This is the first "tight inner loop"
    // that needs to be micro-optimized for speed.  (This one fills out bwtData[]
    // linearly, staying in cache more, so isn't as limited by DRAM access.)
    uint32_t bwtDataCount = 0;
    const auto* huffmanCoding = &huffmanCodings.front();
    for ( uint32_t hh = 0, runPos = 0, symCount = 0, selector = 0; ; ) {
        // Have we reached the end of this huffman group?
        if ( symCount-- == 0 ) {
            // Determine which huffman coding group to use.
            symCount = GROUP_SIZE - 1;
            if ( selector >= selectorsCount ) {
                std::stringstream msg;
                msg << "[BZip2 block data] selector " << selector << " out of maximum range " << selectorsCount;
                throw std::domain_error( std::move( msg ).str() );
            }
            huffmanCoding = &huffmanCodings[selectors[selector]];
            selector++;
        }

        const auto nextSymbol = huffmanCoding->decode( *m_bitReader );
        if ( UNLIKELY( !nextSymbol ) ) [[unlikely]] {
            throw std::domain_error( "[BZip2 block data] Invalid Huffman code" );
        }
        const auto nextSym = *nextSymbol;

        // If this is a repeated run, loop collecting data
        if ( nextSym <= SYMBOL_RUNB ) {
            // If this is the start of a new run, zero out counter
            if ( runPos == 0 ) {
                runPos = 1;
                hh = 0;
            }

            /* Neat trick that saves 1 symbol: instead of or-ing 0 or 1 at
             * each bit position, add 1 or 2 instead. For example,
             * 1011 is 1<<0 + 1<<1 + 2<<2. 1010 is 2<<0 + 2<<1 + 1<<2.
             * You can make any bit pattern that way using 1 less symbol than
             * the basic or 0/1 method (except all bits 0, which would use no
             * symbols, but a run of length 0 doesn't mean anything in this
             * context). Thus space is saved. */
            /* The run length must fit into one block. Check in 64 bit because a crafted sequence of RUNA/RUNB symbols
             * can wrap the 32-bit sum and bypass the block-size check at the end of the run, which would then write
             * far past the block buffer. */
            if ( static_cast<uint64_t>( hh ) + ( static_cast<uint64_t>( runPos ) << nextSym ) > MAX_BLOCK_SIZE ) {
                std::stringstream msg;
                msg << "[BZip2 block data] The run length exceeds the maximum block size " << MAX_BLOCK_SIZE;
                throw std::domain_error( std::move( msg ).str() );
            }
            hh += runPos << nextSym;  // +runPos if RUNA; +2*runPos if RUNB
            runPos <<= 1U;
            continue;
        }

        /* When we hit the first non-run symbol after a run, we now know
         * how many times to repeat the last literal, so append that many
         * copies to our buffer of decoded symbols (bwtData) now. (The last
         * literal used is the one at the head of the mtfSymbol array.) */
        if ( runPos != 0 ) {
            runPos = 0;
            if ( static_cast<uint64_t>( bwtDataCount ) + hh > MAX_BLOCK_SIZE ) {
                std::stringstream msg;
                msg << "[BZip2 block data] bwtDataCount + hh " << static_cast<uint64_t>( bwtDataCount ) + hh
                    << " > " << MAX_BLOCK_SIZE << " maximum block size";
                throw std::domain_error( std::move( msg ).str() );
            }

            const auto uc = symbolToByte[mtfPacked[0] & 0xFFU];
            bwdata.byteCount[uc] += hh;
            while ( hh-- != 0 ) {
                bwdata.bwtData[bwtDataCount++] = uc;
            }
        }

        // Is this the terminating symbol?
        if ( nextSym > symbolCount ) {
            break;
        }

        /* At this point, the symbol we just decoded indicates a new literal
         * character. Subtract one to get the position in the MTF array
         * at which this literal is currently to be found. (Note that the
         * result can't be -1 or 0, because 0 and 1 are RUNA and RUNB.
         * Another instance of the first symbol in the mtf array, position 0,
         * would have been handled as part of a run.) */
        if ( bwtDataCount >= MAX_BLOCK_SIZE ) {
            std::stringstream msg;
            msg << "[BZip2 block data] bwtDataCount " << bwtDataCount
                << " >= " << MAX_BLOCK_SIZE << " maximum block size";
            throw std::domain_error( std::move( msg ).str() );
        }
        const auto uc = symbolToByte[moveToFront( mtfPacked, nextSym - 1 )];

        // We have our literal byte.  Save it into bwtData.
        bwdata.byteCount[uc]++;
        bwdata.bwtData[bwtDataCount++] = uc;
    }

    // Now we know what bwtDataCount is, do a better sanity check on origPtr.
    bwdata.writeCount = bwtDataCount;
    if ( bwdata.origPtr >= bwtDataCount ) {
        std::stringstream msg;
        msg << "[BZip2 block data] origPtr error " << bwdata.origPtr;
        throw std::domain_error( std::move( msg ).str() );
    }

    statistics.durations.createHuffmanTable += rapidgzip::duration( t0 );

    const auto tPrepareStart = rapidgzip::now();
    bwdata.prepare();
    statistics.durations.burrowsWheelerPreparation += rapidgzip::duration( tPrepareStart );

    encodedSizeInBits = bitReader().tell() - encodedOffsetInBits;
}


inline void
Block::BurrowsWheelerTransformData::prepare()
{
    dataCRC = 0xFFFFFFFFL;
    symbolRepeatCount = 0;
    inverseBWT.reset();

    if ( writeCount == 0 ) {
        return;
    }

    /* libsais builds a bi-gram LF table and decodes several chains in parallel, which hides most of the
     * dependent-load latency of the classic one-step chase. The frequency table is already available from
     * the symbol decoding pass. */
    std::array<int32_t, 256> frequencies{};
    for ( size_t i = 0; i < byteCount.size(); ++i ) {
        frequencies[i] = static_cast<int32_t>( byteCount[i] );
    }

    /* libsais requires n + 1 int32 elements of temporary space, which dbuf provides. Inverting in place
     * removes the separate output buffer. On error, bwtData is still the unmodified input because libsais
     * validates and allocates all of its tables before writing, so the chase fallback below stays valid. */
    assert( dbuf.size() >= writeCount + 1 );
    const auto error = libsais_unbwt_cyclic(
        bwtData.data(), bwtData.data(), reinterpret_cast<int32_t*>( dbuf.data() ),
        static_cast<int32_t>( writeCount ), frequencies.data(), static_cast<int32_t>( origPtr ) );
    if ( error == 0 ) {
        inverseBWT = InverseBWTData{};
        std::vector<uint32_t>().swap( dbuf );
        return;
    }

    /* Fall back to the classic one-step LF build if libsais cannot allocate its temporary tables. */
    for ( uint32_t i = 0; i < writeCount; ++i ) {
        dbuf[i] = bwtData[i];
    }

    /**
     * Turn byteCount into cumulative occurrence counts of 0 to n-1.
     * @note This loop is fast because byteCount.size() is 256.
     */
    for ( size_t i = 0, cumulativeCount = 0; i < byteCount.size(); ++i ) {
        const auto newCumulativeCount = cumulativeCount + byteCount[i];
        byteCount[i] = static_cast<uint32_t>( cumulativeCount );
        cumulativeCount = newCumulativeCount;
    }

    // Use occurrence counts to quickly figure out what order dbuf would be in
    // if we sorted it.
    // Using i as position, j as previous character, hh as current character,
    // and uc as run count.
    for ( uint32_t i = 0; i < writeCount; i++ ) {
        const auto uc = static_cast<uint8_t>( dbuf[i] );
        dbuf[byteCount[uc]] |= i << 8U;
        byteCount[uc]++;
    }

    /* Decode first byte by hand to initialize "previous" byte. Note that it
     * doesn't get output, and if the first three characters are identical
     * it doesn't qualify as a run (hence uc=255, which will either wrap
     * to 1 or get reset). */
    writePos = dbuf[origPtr];
    writeCurrent = static_cast<uint8_t>( writePos & 0xFFU );
    writePos >>= 8U;
    writeRun = -1;
}


inline size_t
Block::BurrowsWheelerTransformData::decodeBlock( const size_t nMaxBytesToDecode,
                                                 char*        outputBuffer )
{
    if ( ( outputBuffer == nullptr ) || !hasData() ) {
        return 0;
    }

    size_t nBytesDecoded = 0;
    uint32_t crc = dataCRC;

    if ( inverseBWT ) {
        auto& state = *inverseBWT;
        while ( nBytesDecoded < nMaxBytesToDecode ) {
            if ( state.repeatCount > 0 ) {
                const auto nBytesToRepeat = std::min( state.repeatCount, nMaxBytesToDecode - nBytesDecoded );
                for ( size_t i = 0; i < nBytesToRepeat; ++i ) {
                    outputBuffer[nBytesDecoded++] = static_cast<char>( state.repeatByte );
                }
                state.repeatCount -= nBytesToRepeat;
                continue;
            }

            if ( state.position >= writeCount ) {
                break;
            }

            const auto byte = bwtData[state.position];
            /* Four identical bytes are followed by a count of additional copies. */
            if ( ( state.position + 4 < writeCount )
                 && ( bwtData[state.position + 1] == byte )
                 && ( bwtData[state.position + 2] == byte )
                 && ( bwtData[state.position + 3] == byte ) ) {
                state.repeatByte = byte;
                state.repeatCount = 4 + bwtData[state.position + 4];
                state.position += 5;
            } else {
                outputBuffer[nBytesDecoded++] = static_cast<char>( byte );
                ++state.position;
            }
        }
    } else {
        const auto writeRepeatedSymbols =
            [&, this] ()
            {
                while ( ( symbolRepeatCount > 0 ) && ( nBytesDecoded < nMaxBytesToDecode ) ) {
                    --symbolRepeatCount;
                    outputBuffer[nBytesDecoded++] = static_cast<char>( symbolToRepeat );
                }
            };

        writeRepeatedSymbols();

        while ( ( writeCount > 0 ) && ( nBytesDecoded < nMaxBytesToDecode ) ) {
            writeCount--;

            // Follow sequence vector to undo Burrows-Wheeler transform.
            const auto previous = writeCurrent;
            writePos = dbuf[writePos];
            writeCurrent = static_cast<uint8_t>( writePos & 0xFFU );
            writePos >>= 8U;

            /* Whenever we see 3 consecutive copies of the same byte, the 4th is a repeat count */
            if ( writeRun < 3 ) {
                outputBuffer[nBytesDecoded++] = static_cast<char>( writeCurrent );
                if ( writeCurrent != previous ) {
                    writeRun = 0;
                } else {
                    ++writeRun;
                }
            } else {
                symbolToRepeat = previous;
                symbolRepeatCount = writeCurrent;
                writeRepeatedSymbols();
                writeCurrent = -1;
                writeRun = 0;
            }
        }
    }

    /* The bytes of this call are contiguous in outputBuffer, so the CRC can be computed in one slicing
     * pass after the run expansion instead of interleaved with it. */
    crc = updateCRC32( crc, outputBuffer, nBytesDecoded );
    dataCRC = crc;

    /* decompression of this block completed successfully */
    if ( ( inverseBWT && ( inverseBWT->position >= writeCount ) && ( inverseBWT->repeatCount == 0 ) )
         || ( !inverseBWT && ( writeCount == 0 ) && ( symbolRepeatCount == 0 ) ) ) {
        dataCRC = ~dataCRC;
        if ( dataCRC != headerCRC ) {
            std::stringstream msg;
            msg << "Calculated CRC " << std::hex << dataCRC << " for block mismatches " << headerCRC;
            throw std::runtime_error( std::move( msg ).str() );
        }
    }

    return nBytesDecoded;
}
}  // namespace bzip2
