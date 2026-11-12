/**
 * @file Decodes the entropy-coded part of a bzip2 block, i.e., it undoes the Huffman coding, the zero run-length
 *       encoding (RUNA/RUNB), and the Move-to-Front transform.
 *
 * The result is not the BWT string itself but a list of run records, each a byte value and how often it repeats.
 * The BWT of compressible data mostly consists of such runs, so this is the natural output of the symbol loop,
 * and @ref createLastToFirstMapping can assign the occurrence ranks that the inverse BWT needs for a whole run at once.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <core/BitManipulation.hpp>
#include <core/common.hpp>
#include <core/Error.hpp>

#include "Bzip2Format.hpp"
/* Also provides LIBRAPIDARCHIVE_BZIP2_HAS_SSE2 and the SSE2 intrinsics used by createLastToFirstMapping. */
#include "Bzip2MoveToFront.hpp"

#if defined( __GNUC__ ) || defined( __clang__ )
    #define LIBRAPIDARCHIVE_BZIP2_RESTRICT __restrict__
#elif defined( _MSC_VER )
    #define LIBRAPIDARCHIVE_BZIP2_RESTRICT __restrict
#else
    #define LIBRAPIDARCHIVE_BZIP2_RESTRICT
#endif


namespace bzip2
{
/**
 * @return the next eight bytes as one number whose most significant byte is the first byte, which is the order
 *         in which bzip2 consumes bits.
 */
[[nodiscard]] forceinline uint64_t
loadBigEndian64( const uint8_t* const data ) noexcept
{
    return rapidgzip::byteSwap( rapidgzip::loadUnaligned<uint64_t>( data ) );
}


/**
 * Canonical Huffman code of one bzip2 coding table for a 64-bit bit buffer whose next unread bit is its most
 * significant bit.
 *
 * Codes of up to LUT_BITS bits are decoded with one lookup. The entries only need 16 bits, so that the lookup
 * tables of all six coding tables of a block take 12 KiB and stay in the L1 cache. Longer codes are rare by
 * construction and are found by comparing the left-aligned bits with the largest code of each length.
 */
class SymbolTable
{
public:
    static constexpr uint32_t LUT_BITS = 10;
    /** Lookup results are ( symbol << LENGTH_BITS ) | code length. */
    static constexpr uint32_t LENGTH_BITS = 6;
    static constexpr uint32_t LENGTH_MASK = ( 1U << LENGTH_BITS ) - 1U;
    /** The end-of-block symbol is mapped to this value so that it does not depend on the alphabet size. */
    static constexpr uint32_t END_OF_BLOCK = 511;
    /** Lookup result for bits that do not start with a code of at most LUT_BITS bits. Its length is zero and
     *  its symbol is larger than any real one, which lets callers detect it together with other rare symbols. */
    static constexpr uint16_t NOT_IN_LUT = 0xFFC0U;
    /** Result of @ref decodeLong for bits that do not start with any code. A real entry never has length 0. */
    static constexpr uint32_t INVALID_ENTRY = 0;
    /** Result of @ref decode for bits that do not start with any code. */
    static constexpr uint32_t INVALID_SYMBOL = std::numeric_limits<uint32_t>::max();

public:
    /**
     * @param codeLengths Code lengths in [1, MAX_HUFCODE_BITS] for RUNA, RUNB, the literals, and the end-of-block
     *                    symbol in that order.
     */
    [[nodiscard]] rapidgzip::Error
    initializeFromLengths( const uint8_t* const codeLengths,
                           const size_t         symbolCount ) noexcept
    {
        if ( symbolCount == 0 ) {
            return rapidgzip::Error::EMPTY_ALPHABET;
        }
        if ( symbolCount > MAX_SYMBOLS ) {
            return rapidgzip::Error::INVALID_CODE_LENGTHS;
        }

        std::array<uint16_t, MAX_HUFCODE_BITS + 2> counts{};
        uint32_t minLength = MAX_HUFCODE_BITS;
        uint32_t maxLength = 1;
        for ( size_t symbol = 0; symbol < symbolCount; ++symbol ) {
            const uint32_t length = codeLengths[symbol];
            if ( ( length == 0 ) || ( length > MAX_HUFCODE_BITS ) ) {
                return rapidgzip::Error::INVALID_CODE_LENGTHS;
            }
            ++counts[length];
            minLength = std::min( minLength, length );
            maxLength = std::max( maxLength, length );
        }

        /* Canonical codes: shorter codes are numerically smaller when left-aligned, and codes of the same
         * length are consecutive in symbol order. */
        std::array<uint32_t, MAX_HUFCODE_BITS + 2> nextCode{};
        std::array<uint16_t, MAX_HUFCODE_BITS + 2> nextIndex{};
        uint32_t code = 0;
        uint16_t index = 0;
        for ( uint32_t length = 1; length <= MAX_HUFCODE_BITS; ++length ) {
            m_firstCode[length] = code;
            m_offset[length] = index;
            nextCode[length] = code;
            nextIndex[length] = index;
            index = static_cast<uint16_t>( index + counts[length] );
            code += counts[length];
            /* More codes than a prefix-free code can have for this length. */
            if ( code > ( uint32_t( 1 ) << length ) ) {
                return rapidgzip::Error::INVALID_CODE_LENGTHS;
            }
            /* Largest left-aligned bit pattern that starts with a code of at most this length. For a complete
             * code the shift overflows to zero and the subtraction yields all ones, as intended. */
            m_bound[length] = length < minLength
                              ? 0
                              : ( static_cast<uint64_t>( code ) << ( 64U - length ) ) - 1U;
            code <<= 1U;
        }
        m_offset[MAX_HUFCODE_BITS + 1] = index;
        /* Sentinels that stop the search in decodeLong for bits that are not a code at all. */
        for ( uint32_t length = maxLength + 1; length < m_bound.size(); ++length ) {
            m_bound[length] = std::numeric_limits<uint64_t>::max();
        }
        m_maxLength = static_cast<uint8_t>( maxLength );
        m_longStart = static_cast<uint8_t>( std::max( minLength, LUT_BITS + 1U ) );

        m_lut.fill( NOT_IN_LUT );
        const auto endOfBlock = symbolCount - 1;
        for ( size_t symbol = 0; symbol < symbolCount; ++symbol ) {
            const uint32_t length = codeLengths[symbol];
            const auto value = static_cast<uint16_t>( symbol == endOfBlock ? END_OF_BLOCK : symbol );
            m_permutation[nextIndex[length]++] = value;
            const auto symbolCode = nextCode[length]++;
            if ( length <= LUT_BITS ) {
                const auto entry = static_cast<uint16_t>( ( value << LENGTH_BITS ) | length );
                const auto first = symbolCode << ( LUT_BITS - length );
                const auto last = ( symbolCode + 1U ) << ( LUT_BITS - length );
                std::fill( m_lut.begin() + first, m_lut.begin() + last, entry );
            }
        }
        return rapidgzip::Error::NONE;
    }

    /**
     * @param bits Bit buffer with at least LUT_BITS valid bits starting at the most significant bit.
     * @return ( symbol << LENGTH_BITS ) | code length, or NOT_IN_LUT.
     */
    [[nodiscard]] forceinline uint32_t
    lookup( const uint64_t bits ) const noexcept
    {
        return m_lut[bits >> ( 64U - LUT_BITS )];
    }

    /**
     * Decodes codes for which @ref lookup returned NOT_IN_LUT.
     * @param bits Bit buffer with at least MAX_HUFCODE_BITS valid bits starting at the most significant bit.
     * @return ( symbol << LENGTH_BITS ) | code length, or INVALID_ENTRY if the bits do not start with a code.
     */
    [[nodiscard]] uint32_t
    decodeLong( const uint64_t bits ) const noexcept
    {
        uint32_t length = m_longStart;
        while ( bits > m_bound[length] ) {
            ++length;
        }
        if ( length > m_maxLength ) {
            return INVALID_ENTRY;
        }
        const auto index = static_cast<uint32_t>( bits >> ( 64U - length ) ) - m_firstCode[length];
        return ( static_cast<uint32_t>( m_permutation[m_offset[length] + index] ) << LENGTH_BITS ) | length;
    }

    /**
     * Decodes one symbol bit by bit. This is slow but neither needs buffered input nor reads past the code,
     * so it is used next to the end of the input buffer and of the file.
     * @return the symbol or INVALID_SYMBOL.
     */
    template<typename BitReader>
    [[nodiscard]] uint32_t
    decode( BitReader& bitReader ) const
    {
        uint32_t code = 0;
        for ( uint32_t length = 1; length <= m_maxLength; ++length ) {
            code = ( code << 1U ) | static_cast<uint32_t>( bitReader.template read<1>() );
            const auto index = code - m_firstCode[length];
            const auto count = static_cast<uint32_t>( m_offset[length + 1] - m_offset[length] );
            if ( index < count ) {
                return m_permutation[m_offset[length] + index];
            }
        }
        return INVALID_SYMBOL;
    }

private:
    alignas( 64 ) std::array<uint16_t, 1U << LUT_BITS> m_lut{};
    std::array<uint64_t, MAX_HUFCODE_BITS + 2> m_bound{};
    std::array<uint32_t, MAX_HUFCODE_BITS + 2> m_firstCode{};
    /** Index of the first symbol with a given code length in m_permutation. */
    std::array<uint16_t, MAX_HUFCODE_BITS + 2> m_offset{};
    /** Symbols sorted by code length and then by value. */
    std::array<uint16_t, MAX_SYMBOLS> m_permutation{};
    uint8_t m_maxLength{ 0 };
    uint8_t m_longStart{ 0 };
};


/** A record is a byte value followed by its repeat count in [0, 255]. */
static constexpr size_t RUN_RECORD_SIZE = 2;
/** Every record but the first one stands for at least one byte, so a valid block cannot have more. */
static constexpr size_t MAX_RUN_RECORDS = MAX_BLOCK_SIZE + 1;
/** The record count is only checked once per group of symbols. */
static constexpr size_t RUN_RECORD_BUFFER_SIZE = RUN_RECORD_SIZE * ( MAX_RUN_RECORDS + GROUP_SIZE + 8 );


enum class SymbolDecoderStatus : uint8_t
{
    NEED_INPUT,
    END_OF_BLOCK,
    INVALID_CODE,
    BLOCK_TOO_LARGE,
    OUT_OF_SELECTORS,
};


/**
 * Everything that is carried from one group of symbols to the next. The kernels copy it into local variables
 * and write it back when they return, which is always at a group boundary unless the block ends.
 */
struct SymbolDecoderState
{
    /** The Move-to-Front list. It contains the byte values instead of indexes into the symbol map. */
    alignas( 64 ) PackedMTF mtf{};
    /** The record of the byte that currently is at the front of the list. Records are only appended. */
    uint8_t* record{ nullptr };
    /** Records behind this are an error. The buffer must extend to RUN_RECORD_BUFFER_SIZE. */
    const uint8_t* recordLimit{ nullptr };
    /** Repeat count of *record. */
    uint32_t length{ 0 };
    /** How many bytes the next RUNA symbol stands for. RUNB stands for twice as many. */
    uint32_t runWeight{ 1 };
    /** Number of different byte values in the block. */
    uint32_t symbolCount{ 0 };
    const uint8_t* selector{ nullptr };
    const uint8_t* selectorEnd{ nullptr };
    const SymbolTable* tables{ nullptr };
};


/**
 * Splits a record whose repeat count does not fit into one byte anymore into several records.
 * @return false if the block cannot be valid anymore because it would have more than MAX_BLOCK_SIZE bytes.
 */
[[nodiscard]] forceinline bool
splitLongRecord( uint8_t*&            record,
                 const uint8_t* const recordLimit,
                 uint32_t&            length ) noexcept
{
    if ( ( length > MAX_BLOCK_SIZE ) || ( record > recordLimit )
         || ( static_cast<size_t>( recordLimit - record ) < RUN_RECORD_SIZE * ( length / 255U + 1U ) ) ) {
        return false;
    }

    const auto byte = record[0];
    while ( length > 255U ) {
        record[0] = byte;
        record[1] = 255;
        record += RUN_RECORD_SIZE;
        length -= 255U;
    }
    record[0] = byte;
    record[1] = static_cast<uint8_t>( length );
    return true;
}


/** Longest possible group plus the bytes that the unconditional bit buffer refills may read ahead. */
static constexpr size_t SYMBOL_KERNEL_MINIMUM_INPUT = 144;
static_assert( SYMBOL_KERNEL_MINIMUM_INPUT >= ( GROUP_SIZE * MAX_HUFCODE_BITS + 63 ) / 8 + 1 + sizeof( uint64_t ),
               "A whole group must be decodable without bounds checks." );


#define LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT 0
#define LIBRAPIDARCHIVE_BZIP2_KERNEL_NAME decodeSymbolsPortable
#define LIBRAPIDARCHIVE_BZIP2_KERNEL_TARGET
#include "Bzip2SymbolKernel.hpp"
#undef LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT
#undef LIBRAPIDARCHIVE_BZIP2_KERNEL_NAME
#undef LIBRAPIDARCHIVE_BZIP2_KERNEL_TARGET


/**
 * Decodes the next group of at most GROUP_SIZE symbols with the bit reader. It does not need buffered input
 * and is used to get over the end of the input buffer and for the last bytes of a file.
 *
 * @return NEED_INPUT if the block has not ended yet.
 */
template<typename BitReader>
[[nodiscard]] SymbolDecoderStatus
decodeSymbolGroup( SymbolDecoderState& state,
                   BitReader&          bitReader )
{
    if ( state.selector == state.selectorEnd ) {
        return SymbolDecoderStatus::OUT_OF_SELECTORS;
    }
    if ( state.record > state.recordLimit ) {
        return SymbolDecoderStatus::BLOCK_TOO_LARGE;
    }

    const auto& table = state.tables[*state.selector++];
    for ( int i = 0; i < GROUP_SIZE; ++i ) {
        const auto symbol = table.decode( bitReader );
        if ( symbol == SymbolTable::INVALID_SYMBOL ) {
            return SymbolDecoderStatus::INVALID_CODE;
        }
        if ( symbol == SymbolTable::END_OF_BLOCK ) {
            return SymbolDecoderStatus::END_OF_BLOCK;
        }

        if ( symbol <= SYMBOL_RUNB ) {
            /* RUNA and RUNB are the digits one and two of a bijective base-2 number, least significant
             * digit first. The weight cannot overflow because the length is checked after each digit. */
            state.length += state.runWeight << symbol;
            state.runWeight <<= 1U;
            state.record[1] = static_cast<uint8_t>( state.length );
            if ( ( state.length > 255U ) && !splitLongRecord( state.record, state.recordLimit, state.length ) ) {
                return SymbolDecoderStatus::BLOCK_TOO_LARGE;
            }
        } else {
            const auto byte = moveToFront( state.mtf, symbol - 1U );
            state.record += RUN_RECORD_SIZE;
            state.record[0] = byte;
            state.record[1] = 1;
            state.length = 1;
            state.runWeight = 1;
        }
    }
    return SymbolDecoderStatus::NEED_INPUT;
}


/** The last record may write up to 255 entries plus the eight entries of an unconditional vector store, and
 *  the inverse BWT uses one entry behind the last one. */
static constexpr size_t LAST_TO_FIRST_MAPPING_SIZE = MAX_BLOCK_SIZE + 256 + 16;
static constexpr uint32_t LAST_TO_FIRST_MAPPING_TOO_LARGE = std::numeric_limits<uint32_t>::max();


/**
 * Expands the run records into one entry per byte of the BWT string. The entry of the i-th byte holds the
 * byte in its lowest eight bits and above that how many times this byte value occurs before position i.
 * Together with the cumulative byte counts this is the last-to-first mapping that @ref InverseBWT follows.
 *
 * Because the ranks of a run are consecutive, a run is written with a few vector stores instead of with one
 * dependent load and store of the counter per byte.
 *
 * @param lastToFirstMapping Must have room for LAST_TO_FIRST_MAPPING_SIZE entries.
 * @return the size of the BWT string or LAST_TO_FIRST_MAPPING_TOO_LARGE if it is longer than MAX_BLOCK_SIZE.
 */
[[nodiscard]] inline uint32_t
createLastToFirstMapping( const uint8_t* LIBRAPIDARCHIVE_BZIP2_RESTRICT  records,
                          const size_t                                   recordCount,
                          uint32_t* LIBRAPIDARCHIVE_BZIP2_RESTRICT const lastToFirstMapping,
                          std::array<uint32_t, 256>&                     byteCounts ) noexcept
{
    std::array<uint32_t, 256> counts{};
    auto* target = lastToFirstMapping;
    const auto* const limit = lastToFirstMapping + MAX_BLOCK_SIZE;

#if LIBRAPIDARCHIVE_BZIP2_HAS_SSE2
    const auto offsets0 = _mm_setr_epi32( 0 << 8, 1 << 8, 2 << 8, 3 << 8 );
    const auto offsets1 = _mm_setr_epi32( 4 << 8, 5 << 8, 6 << 8, 7 << 8 );
    const auto step = _mm_set1_epi32( 8 << 8 );
#endif

    for ( size_t i = 0; i < recordCount; ++i ) {
        const uint32_t byte = records[RUN_RECORD_SIZE * i];
        const uint32_t count = records[RUN_RECORD_SIZE * i + 1];
        const auto rank = counts[byte];
        counts[byte] = rank + count;
        const auto first = ( rank << 8U ) | byte;

        /* Always write eight entries. Those behind the run are overwritten by the next record. */
    #if LIBRAPIDARCHIVE_BZIP2_HAS_SSE2
        auto entries0 = _mm_add_epi32( _mm_set1_epi32( static_cast<int>( first ) ), offsets0 );
        auto entries1 = _mm_add_epi32( _mm_set1_epi32( static_cast<int>( first ) ), offsets1 );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( target ), entries0 );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( target + 4 ), entries1 );
        if ( UNLIKELY( count > 8 ) ) [[unlikely]] {
            for ( uint32_t k = 8; k < count; k += 8 ) {
                entries0 = _mm_add_epi32( entries0, step );
                entries1 = _mm_add_epi32( entries1, step );
                _mm_storeu_si128( reinterpret_cast<__m128i*>( target + k ), entries0 );
                _mm_storeu_si128( reinterpret_cast<__m128i*>( target + k + 4 ), entries1 );
            }
        }
    #else
        for ( uint32_t k = 0; k < 8; ++k ) {
            target[k] = first + ( k << 8U );
        }
        for ( uint32_t k = 8; k < count; ++k ) {
            target[k] = first + ( k << 8U );
        }
    #endif

        target += count;
        if ( UNLIKELY( target > limit ) ) [[unlikely]] {
            return LAST_TO_FIRST_MAPPING_TOO_LARGE;
        }
    }

    byteCounts = counts;
    return static_cast<uint32_t>( target - lastToFirstMapping );
}
}  // namespace bzip2
