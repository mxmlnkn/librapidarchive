/**
 * @file Focused benchmark for the uncompressed (stored) block paths in rapidgzip::deflate::Block::read.
 *
 * The four cases cover the three fast paths and the fallback:
 *  1. large        A single 65535 B stored block on a fresh Block. m_uncompressedSize >= MAX_WINDOW_SIZE,
 *                  so the block is read directly at window position 0.
 *  2. marker       A marker-producing fixed block followed by a 30000 B stored block. The marker distance is
 *                  5000, so the stored block keeps the newest 2768 marker-free bytes at the upper end of
 *                  the physical window and reads at position 0.
 *  3. fallback     A marker-producing fixed block followed by a 30000 B stored block. The marker distance is
 *                  0, which is too small for the marker fast path, so readInternalUncompressed runs.
 *  4. no-marker    A real 32 KiB initial window followed by a 30000 B stored block. The marker-free window
 *                  uses the wrap-aware lastBuffers copy.
 *
 * The timed region starts after the preceding block and the stored block header have been set up, so the
 * reported MB/s is the stored-block read path. Run with --crc to include a CRC of the returned bytes, which
 * shows the difference between the block copy and the checksum done by the chunk pipeline.
 *
 * Usage:
 *   taskset -c 0-7 build/src/benchmarks/benchmarkStoredBlocks
 *   taskset -c 0-7 build/src/benchmarks/benchmarkStoredBlocks --case all --iterations 15 --warmup 3 --crc
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <core/common.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/crc32.hpp>
#include <rapidgzip/gzip/deflate.hpp>
#include <rapidgzip/gzip/definitions.hpp>


namespace
{
using namespace rapidgzip;


class DeflateBitWriter
{
public:
    void
    writeBits( uint32_t value,
               uint8_t  count )
    {
        m_bitBuffer |= static_cast<uint64_t>( value ) << m_bitCount;
        m_bitCount += count;
        while ( m_bitCount >= 8 ) {
            m_data.push_back( static_cast<uint8_t>( m_bitBuffer & 0xFFU ) );
            m_bitBuffer >>= 8U;
            m_bitCount -= 8;
        }
    }

    void
    writeCode( uint32_t code,
               uint8_t  count )
    {
        uint32_t reversed{ 0 };
        for ( uint8_t i = 0; i < count; ++i ) {
            reversed |= ( ( code >> ( count - 1 - i ) ) & 1U ) << i;
        }
        writeBits( reversed, count );
    }

    void
    alignToByte()
    {
        if ( ( m_bitCount % 8 ) != 0 ) {
            writeBits( 0, static_cast<uint8_t>( 8 - ( m_bitCount % 8 ) ) );
        }
    }

    void
    writeStoredBlock( const std::vector<uint8_t>& data,
                      const bool                  isFinal )
    {
        writeBits( isFinal ? 1U : 0U, 1 );
        writeBits( 0, 2 );  /* BTYPE = stored */
        alignToByte();
        const auto size = static_cast<uint16_t>( data.size() );
        writeBits( size, 16 );
        writeBits( static_cast<uint16_t>( ~size ), 16 );
        for ( const auto byte : data ) {
            writeBits( byte, 8 );
        }
    }

    [[nodiscard]] std::vector<uint8_t>
    finish()
    {
        if ( m_bitCount > 0 ) {
            m_data.push_back( static_cast<uint8_t>( m_bitBuffer & 0xFFU ) );
            m_bitBuffer = 0;
            m_bitCount = 0;
        }
        return m_data;
    }

private:
    std::vector<uint8_t> m_data;
    uint64_t m_bitBuffer{ 0 };
    uint8_t m_bitCount{ 0 };
};


void
writeFixedLiteralOrLength( DeflateBitWriter& writer,
                           const uint16_t    symbol )
{
    if ( symbol <= 143 ) {
        writer.writeCode( 0x30U + symbol, 8 );
    } else if ( symbol <= 255 ) {
        writer.writeCode( 0x190U + ( symbol - 144 ), 9 );
    } else if ( symbol <= 279 ) {
        writer.writeCode( symbol - 256, 7 );
    } else {
        writer.writeCode( 0xC0U + ( symbol - 280 ), 8 );
    }
}


void
writeFixedDistance( DeflateBitWriter& writer,
                    const uint16_t    symbol )
{
    writer.writeCode( symbol, 5 );
}


/* A fixed block that starts with a distance-1 match into the unknown window, so the match emits markers,
 * followed by literalCount bytes. */
void
writeMarkerBlock( DeflateBitWriter& writer,
                  const size_t      literalCount )
{
    writer.writeBits( 0, 1 );  /* BFINAL */
    writer.writeBits( 1, 2 );  /* BTYPE = fixed Huffman */
    writeFixedLiteralOrLength( writer, 285 );  /* length 258 */
    writeFixedDistance( writer, 0 );           /* distance 1 */
    for ( size_t i = 0; i < literalCount; ++i ) {
        writeFixedLiteralOrLength( writer, static_cast<uint16_t>( 'A' + ( i % 26 ) ) );
    }
    writeFixedLiteralOrLength( writer, 256 );  /* end of block */
}


[[nodiscard]] std::vector<uint8_t>
createPayload( const size_t size )
{
    std::vector<uint8_t> payload( size );
    for ( size_t i = 0; i < size; ++i ) {
        payload[i] = static_cast<uint8_t>( ( i * 131U + 7U ) & 0xFFU );
    }
    return payload;
}


enum class Case
{
    LARGE,
    MARKER,
    FALLBACK,
    NO_MARKER,
};


[[nodiscard]] std::string_view
toString( const Case testCase )
{
    switch ( testCase )
    {
    case Case::LARGE: return "large";
    case Case::MARKER: return "marker";
    case Case::FALLBACK: return "fallback";
    case Case::NO_MARKER: return "no-marker";
    }
    return "unknown";
}


constexpr size_t LARGE_STORED_SIZE = 65535;
constexpr size_t SMALL_STORED_SIZE = 30000;
constexpr size_t MARKER_DISTANCE = 5000;


struct TestCase
{
    Case testCase{ Case::LARGE };
    std::vector<uint8_t> stream;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> initialWindow;
    bool headerIsSecondBlock{ false };
};


[[nodiscard]] TestCase
createTestCase( const Case testCase )
{
    TestCase result;
    result.testCase = testCase;

    DeflateBitWriter writer;
    switch ( testCase )
    {
    case Case::LARGE:
        result.payload = createPayload( LARGE_STORED_SIZE );
        writer.writeStoredBlock( result.payload, true );
        break;
    case Case::MARKER:
        result.payload = createPayload( SMALL_STORED_SIZE );
        writeMarkerBlock( writer, MARKER_DISTANCE );
        writer.writeStoredBlock( result.payload, true );
        result.headerIsSecondBlock = true;
        break;
    case Case::FALLBACK:
        result.payload = createPayload( SMALL_STORED_SIZE );
        writeMarkerBlock( writer, 0 );
        writer.writeStoredBlock( result.payload, true );
        result.headerIsSecondBlock = true;
        break;
    case Case::NO_MARKER:
        result.payload = createPayload( SMALL_STORED_SIZE );
        writer.writeStoredBlock( result.payload, true );
        result.initialWindow = createPayload( deflate::MAX_WINDOW_SIZE );
        break;
    }

    result.stream = writer.finish();
    /* The compressed paths peek ahead, so keep enough trailing bytes for the last group. */
    result.stream.insert( result.stream.end(), 8, 0 );
    return result;
}


struct ReadResult
{
    std::vector<uint8_t> bytes;
    double seconds{ 0 };
};


[[nodiscard]] ReadResult
runOnce( const TestCase& testCase,
         const bool      computeChecksum )
{
    gzip::BitReader bitReader(
        std::make_unique<BufferViewFileReader>( testCase.stream.data(), testCase.stream.size() ) );
    auto block = std::make_unique<deflate::Block<> >();
    if ( !testCase.initialWindow.empty() ) {
        block->setInitialWindow( { testCase.initialWindow.data(), testCase.initialWindow.size() } );
    }

    if ( testCase.headerIsSecondBlock ) {
        const auto firstHeaderError = block->readHeader( bitReader );
        if ( firstHeaderError != Error::NONE ) {
            throw std::logic_error( "Failed to read the first deflate block header: " + toString( firstHeaderError ) );
        }
        while ( !block->eob() ) {
            const auto [view, error] = block->read( bitReader, std::numeric_limits<size_t>::max() );
            if ( error != Error::NONE ) {
                throw std::logic_error( "Failed to decode the marker block: " + toString( error ) );
            }
            (void)view;
        }
    }

    const auto headerError = block->readHeader( bitReader );
    if ( headerError != Error::NONE ) {
        throw std::logic_error( "Failed to read the stored block header: " + toString( headerError ) );
    }

    const auto start = now();
    const auto [view, error] = block->read( bitReader, std::numeric_limits<size_t>::max() );
    if ( computeChecksum ) {
        uint32_t crc{ 0 };
        for ( const auto& buffer : view.data ) {
            crc = updateCRC32( crc, reinterpret_cast<const char*>( buffer.data() ), buffer.size() );
        }
        for ( const auto& buffer : view.dataWithMarkers ) {
            for ( const auto symbol : buffer ) {
                crc = updateCRC32( crc, static_cast<uint8_t>( symbol ) );
            }
        }
        /* The checksum itself is not validated here; the byte comparison below covers the data. */
        static_cast<void>( crc );
    }
    const auto seconds = duration( start );
    if ( error != Error::NONE ) {
        throw std::logic_error( "Failed to decode the stored block: " + toString( error ) );
    }

    ReadResult result;
    result.seconds = seconds;
    result.bytes.reserve( view.size() );
    for ( const auto& buffer : view.data ) {
        result.bytes.insert( result.bytes.end(), buffer.begin(), buffer.end() );
    }
    for ( const auto& buffer : view.dataWithMarkers ) {
        for ( const auto symbol : buffer ) {
            if ( symbol > 255U ) {
                throw std::logic_error( "The stored block produced a marker byte!" );
            }
            result.bytes.push_back( static_cast<uint8_t>( symbol ) );
        }
    }

    return result;
}
}  // namespace


int
main( int    argc,
      char** argv )
{
    std::vector<Case> cases{ Case::LARGE, Case::MARKER, Case::FALLBACK, Case::NO_MARKER };
    size_t iterations{ 10 };
    size_t warmupIterations{ 2 };
    bool computeChecksum{ false };

    for ( int i = 1; i < argc; ++i ) {
        const std::string argument = argv[i];
        if ( ( argument == "--case" ) && ( i + 1 < argc ) ) {
            const std::string caseName = argv[++i];
            cases.clear();
            for ( const auto testCase : { Case::LARGE, Case::MARKER, Case::FALLBACK, Case::NO_MARKER } ) {
                if ( caseName == toString( testCase ) ) {
                    cases.push_back( testCase );
                }
            }
            if ( cases.empty() ) {
                throw std::invalid_argument( "Unknown case: " + caseName );
            }
        } else if ( ( argument == "--iterations" ) && ( i + 1 < argc ) ) {
            iterations = std::stoull( argv[++i] );
        } else if ( ( argument == "--warmup" ) && ( i + 1 < argc ) ) {
            warmupIterations = std::stoull( argv[++i] );
        } else if ( argument == "--crc" ) {
            computeChecksum = true;
        } else {
            std::cerr << "Unknown argument: " << argument << "\n";
            return 2;
        }
    }

    for ( const auto testCase : cases ) {
        const auto data = createTestCase( testCase );
        std::cout << "# case=" << toString( testCase )
                  << " stream=" << data.stream.size()
                  << " payload=" << data.payload.size()
                  << " crc=" << ( computeChecksum ? "on" : "off" ) << "\n";

        const auto expected = runOnce( data, computeChecksum );
        if ( expected.bytes != data.payload ) {
            throw std::logic_error( "Decoded stored-block bytes do not match the payload!" );
        }

        for ( size_t i = 0; i < warmupIterations; ++i ) {
            (void)runOnce( data, computeChecksum );
        }

        for ( size_t i = 0; i < iterations; ++i ) {
            const auto result = runOnce( data, computeChecksum );
            if ( result.bytes != data.payload ) {
                throw std::logic_error( "Timed run produced different stored-block bytes!" );
            }
            std::cout << "case=" << toString( testCase )
                      << " iteration=" << i
                      << " bytes=" << result.bytes.size()
                      << " seconds=" << result.seconds
                      << " MBps=" << ( static_cast<double>( result.bytes.size() ) / 1e6 / result.seconds )
                      << "\n";
        }
    }

    return 0;
}
