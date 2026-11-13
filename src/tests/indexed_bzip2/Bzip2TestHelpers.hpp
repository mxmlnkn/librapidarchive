#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <core/TestHelpers.hpp>
#include <indexed_bzip2/Bzip2Format.hpp>
#include <indexed_bzip2/Bzip2SymbolDecoder.hpp>
#ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
#include <indexed_bzip2/Bzip2Compressor.hpp>
#endif


namespace bzip2_test
{
using namespace rapidgzip;


/** Write a bzip2 stream without depending on an external bzip2 executable. */
inline void
compressFile( const std::filesystem::path& decodedPath,
              const std::filesystem::path& encodedPath,
              const int                    level = 9 )
{
#ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
    const auto data = readFile<std::vector<uint8_t> >( decodedPath );
    const auto compressed = indexed_bzip2::compressBzip2Stream( data.data(), data.size(), level );
    std::ofstream file( encodedPath, std::ios_base::out | std::ios_base::binary );
    file.write( reinterpret_cast<const char*>( compressed.data() ),
                static_cast<std::streamsize>( compressed.size() ) );
    REQUIRE( file.good() );
#else
    const auto command = "bzip2 -" + std::to_string( level ) + " -c -- '" + decodedPath.string()
                         + "' > '" + encodedPath.string() + "'";
    REQUIRE( std::system( command.c_str() ) == 0 );
#endif
}

/** bzip2 -c of the single byte 'A'. A single data block and an EOS block with its stream CRC. */
inline const std::vector<uint8_t> SINGLE_BYTE_COMPRESSED_STREAM = {
    0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x81, 0xb0,
    0x2d, 0x8b, 0x00, 0x00, 0x00, 0x04, 0x00, 0x20, 0x00, 0x20, 0x00, 0x21,
    0x18, 0x46, 0x82, 0xee, 0x48, 0xa7, 0x0a, 0x12, 0x10, 0x36, 0x05, 0xb1,
    0x60
};

/**
 * @return Bit offsets of every occurrence of the 48-bit @p magic in @p compressed, in increasing order.
 *         Bzip2 blocks are not byte-aligned, so the whole bit string has to be scanned.
 */
[[nodiscard]] inline std::vector<size_t>
findMagicBitOffsets( const std::vector<uint8_t>& compressed,
                     const uint64_t              magic )
{
    std::vector<size_t> offsets;
    uint64_t window{ 0 };
    const auto nBits = compressed.size() * 8U;
    for ( size_t bit = 0; bit < nBits; ++bit ) {
        window = ( ( window << 1U ) | ( ( compressed[bit / 8U] >> ( 7U - bit % 8U ) ) & 1U ) )
                 & ( ( uint64_t( 1 ) << bzip2::MAGIC_BITS_SIZE ) - 1U );
        if ( ( bit + 1 >= bzip2::MAGIC_BITS_SIZE ) && ( window == magic ) ) {
            offsets.push_back( bit + 1 - bzip2::MAGIC_BITS_SIZE );
        }
    }
    return offsets;
}


inline void
flipBit( std::vector<uint8_t>& compressed,
         const size_t          bitOffset )
{
    REQUIRE( bitOffset / 8U < compressed.size() );
    compressed[bitOffset / 8U] ^= static_cast<uint8_t>( 1U << ( 7U - bitOffset % 8U ) );
}


/** The stored CRC directly follows the 48-bit block or EOS magic. */
inline void
corruptStoredCRCAfterMagic( std::vector<uint8_t>& compressed,
                            const size_t          magicBitOffset )
{
    flipBit( compressed, magicBitOffset + bzip2::MAGIC_BITS_SIZE );
}


/** Corrupt the stream CRC of the last EOS block, which is the last EOS magic in a single or concatenated stream. */
inline void
corruptStreamCRC( std::vector<uint8_t>& compressed )
{
    const auto eosOffsets = findMagicBitOffsets( compressed, bzip2::MAGIC_BITS_EOS );
    REQUIRE( !eosOffsets.empty() );
    if ( eosOffsets.empty() ) {
        return;
    }
    corruptStoredCRCAfterMagic( compressed, eosOffsets.back() );
}


/**
 * Corrupt the stored block CRC of the @p blockIndex-th data block. The block magic is unique enough that the
 * scan cannot confuse it with data for the deterministic test streams used here.
 */
inline void
corruptBlockCRC( std::vector<uint8_t>& compressed,
                 const size_t          blockIndex = 0 )
{
    const auto blockOffsets = findMagicBitOffsets( compressed, bzip2::MAGIC_BITS_BLOCK );
    REQUIRE( blockOffsets.size() > blockIndex );
    if ( blockOffsets.size() <= blockIndex ) {
        return;
    }
    corruptStoredCRCAfterMagic( compressed, blockOffsets[blockIndex] );
}


template<typename Reader>
[[nodiscard]] inline std::vector<char>
readAll( Reader& reader )
{
    std::vector<char> result;
    std::vector<char> buffer( 64UL << 10U );
    while ( const auto nBytesRead = reader.read( buffer.data(), buffer.size() ) ) {
        result.insert( result.end(), buffer.data(), buffer.data() + nBytesRead );
    }
    return result;
}


/**
 * @return The exception message if reading to EOF throws, or an empty string if the stream decoded successfully.
 */
template<typename Reader>
[[nodiscard]] inline std::string
readToEndAndCaptureMessage( Reader& reader )
{
    try {
        static_cast<void>( readAll( reader ) );
    } catch ( const std::exception& exception ) {
        return exception.what();
    }
    return {};
}


inline void
requireBlockCRCError( const std::string& message )
{
    REQUIRE( message.find( "CRC" ) != std::string::npos );
    /* The stream CRC is accumulated from the calculated block CRCs, so a corrupted stored block CRC would
     * otherwise go unnoticed. The block-specific text proves that the per-block comparison itself ran. */
    REQUIRE( message.find( "block mismatches" ) != std::string::npos );
}


inline void
requireStreamCRCError( const std::string& message )
{
    REQUIRE( message.find( "Stream CRC" ) != std::string::npos );
}


/**
 * Runs the same single-block checksum checks for a serial and a parallel reader.
 * @param makeReader Callable taking the compressed bytes and returning a std::unique_ptr to the reader.
 */
template<typename Factory>
void
checkSingleBlockCRCs( Factory makeReader )
{
    const auto& compressed = SINGLE_BYTE_COMPRESSED_STREAM;
    {
        auto reader = makeReader( compressed );
        REQUIRE( readAll( *reader ) == std::vector<char>{ 'A' } );
    }

    auto blockCorrupted = compressed;
    corruptBlockCRC( blockCorrupted );
    {
        auto reader = makeReader( blockCorrupted );
        requireBlockCRCError( readToEndAndCaptureMessage( *reader ) );
    }

    auto streamCorrupted = compressed;
    corruptStreamCRC( streamCorrupted );
    {
        auto reader = makeReader( streamCorrupted );
        requireStreamCRCError( readToEndAndCaptureMessage( *reader ) );
    }
}


/**
 * Forces one symbol kernel and restores the automatic choice afterwards. All supported kernels decode the same
 * code paths around the CRC checks, so corrupted inputs must be rejected independently of the selected kernel.
 */
class ScopedSymbolKernel
{
public:
    explicit
    ScopedSymbolKernel( const bzip2::SymbolKernel kernel )
    {
        bzip2::setSymbolKernel( kernel );
    }

    ~ScopedSymbolKernel()
    {
        bzip2::setSymbolKernel( bzip2::SymbolKernel::AUTOMATIC );
    }

    ScopedSymbolKernel( const ScopedSymbolKernel& ) = delete;
    ScopedSymbolKernel& operator=( const ScopedSymbolKernel& ) = delete;
};


[[nodiscard]] inline std::vector<bzip2::SymbolKernel>
supportedSymbolKernels()
{
    std::vector<bzip2::SymbolKernel> kernels{ bzip2::SymbolKernel::PORTABLE };
    if ( bzip2::isSupported( bzip2::SymbolKernel::SSE41 ) ) {
        kernels.emplace_back( bzip2::SymbolKernel::SSE41 );
    }
    if ( bzip2::isSupported( bzip2::SymbolKernel::AVX2 ) ) {
        kernels.emplace_back( bzip2::SymbolKernel::AVX2 );
    }
    if ( bzip2::isSupported( bzip2::SymbolKernel::NEON ) ) {
        kernels.emplace_back( bzip2::SymbolKernel::NEON );
    }
    return kernels;
}


/**
 * Runs the same multi-block checksum checks for a serial and a parallel reader.
 * @param makeReader Callable taking the compressed bytes and returning a std::unique_ptr to the reader.
 */
template<typename Factory>
void
checkMultiBlockCRCs( const std::vector<uint8_t>& compressed,
                     const std::vector<char>&    expected,
                     Factory                     makeReader )
{
    const auto blockOffsets = findMagicBitOffsets( compressed, bzip2::MAGIC_BITS_BLOCK );
    REQUIRE( blockOffsets.size() >= 2 );

    for ( const auto kernel : supportedSymbolKernels() ) {
        const ScopedSymbolKernel kernelScope{ kernel };

        {
            auto reader = makeReader( compressed );
            REQUIRE( readAll( *reader ) == expected );
        }

        for ( const auto blockIndex : { size_t{ 0 }, blockOffsets.size() - 1 } ) {
            auto corrupted = compressed;
            corruptBlockCRC( corrupted, blockIndex );
            auto reader = makeReader( corrupted );
            requireBlockCRCError( readToEndAndCaptureMessage( *reader ) );
        }

        auto corrupted = compressed;
        corruptStreamCRC( corrupted );
        auto reader = makeReader( corrupted );
        requireStreamCRCError( readToEndAndCaptureMessage( *reader ) );
    }
}
}  // namespace bzip2_test
