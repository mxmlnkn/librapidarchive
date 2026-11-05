/**
 * @file Focused benchmark for the custom deflate::Block inflate path.
 *
 * This path is used when ISA-L is disabled and, even with ISA-L enabled, for chunks without a resolved
 * back-reference window and for the back-reference usage tracking that builds the sparse seek index.
 * It intentionally bypasses ChunkData and the parallel pipeline so that a change to the custom Huffman
 * decoder is measured directly. One line per iteration is printed; the caller computes medians.
 *
 * Modes:
 *   no-window-first  Fresh Block, no initial window, 32 KiB. Upper bound on the marker-window cost.
 *   no-window-chunk  Fresh Block, no initial window, 128 KiB. The first chunk as decoded by the chunk
 *                    pipeline, which switches to the uint8 window after 32 KiB.
 *   no-window-full   Fresh Block, no initial window, whole stream.
 *   window-full      Fresh Block with a real 32 KiB initial window, whole stream.
 *   usage            Fresh Block with setTrackBackreferences, 32 KiB. This is what getUsedWindowSymbols does.
 *   gzip-reader      GzipReader with StoppingPoint::END_OF_BLOCK_HEADER for end-to-end context.
 *
 * Usage:
 *   cmake --build . --target benchmarkCustomInflate &&
 *   taskset -c 5 src/benchmarks/benchmarkCustomInflate [--mode all|...] [--data all|base64|backrefs]
 *                                                      [--size 64Mi] [--iterations 5] [--warmup 1]
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <core/common.hpp>
#include <core/DataGenerators.hpp>
#include <core/FileRanges.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/GzipReader.hpp>
#include <rapidgzip/gzip/crc32.hpp>
#include <rapidgzip/gzip/deflate.hpp>
#include <rapidgzip/gzip/zlib.hpp>


namespace rapidgzip
{
/* Matches ALLOCATION_CHUNK_SIZE in the chunk pipeline. */
constexpr size_t FIRST_CHUNK_SIZE = 128_Ki;

enum class DecodeMode
{
    NO_WINDOW_FIRST,
    NO_WINDOW_CHUNK,
    NO_WINDOW_FULL,
    WINDOW_FULL,
    USAGE_TRACKING,
    GZIP_READER,
};


struct DecodeResult
{
    size_t decodedBytes{ 0 };
    uint32_t crc32{ 0 };
    size_t blockCount{ 0 };
    size_t backreferenceCount{ 0 };
    bool containsMarkers{ false };
};


[[nodiscard]] std::string_view
toString( const DecodeMode mode )
{
    switch ( mode )
    {
    case DecodeMode::NO_WINDOW_FIRST: return "no-window-first";
    case DecodeMode::NO_WINDOW_CHUNK: return "no-window-chunk";
    case DecodeMode::NO_WINDOW_FULL: return "no-window-full";
    case DecodeMode::WINDOW_FULL: return "window-full";
    case DecodeMode::USAGE_TRACKING: return "usage";
    case DecodeMode::GZIP_READER: return "gzip-reader";
    }
    return "unknown";
}


[[nodiscard]] uint32_t
crc32OfView( uint32_t                                   crc,
             const std::array<VectorView<uint8_t>, 2>&  data,
             const std::array<VectorView<uint16_t>, 2>& dataWithMarkers )
{
    for ( const auto& buffer : data ) {
        crc = updateCRC32( crc, reinterpret_cast<const char*>( buffer.data() ), buffer.size() );
    }
    for ( const auto& buffer : dataWithMarkers ) {
        for ( const auto symbol : buffer ) {
            /* Only the low byte is meaningful. Markers only exist for data that was never decoded and this
             * benchmark always starts at the stream start, so no marker may survive here. */
            if ( symbol > 255U ) {
                throw std::logic_error( "Unexpected marker byte in benchmark input!" );
            }
            crc = updateCRC32( crc, static_cast<uint8_t>( symbol ) );
        }
    }
    return crc;
}


[[nodiscard]] DecodeResult
decodeWithBlock( const std::vector<uint8_t>& compressed,
                 const DecodeMode            mode,
                 const size_t                maxBytes,
                 const VectorView<uint8_t>   window,
                 const bool                  computeChecksum )
{
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( compressed.data(), compressed.size() ) );

    /* Allocate on the heap: Block is ~200 KiB and contains two window buffers. */
    auto block = std::make_unique<deflate::Block<>>();
    if ( !window.empty() ) {
        block->setInitialWindow( window );
    }
    if ( mode == DecodeMode::USAGE_TRACKING ) {
        block->setTrackBackreferences( true );
    }

    DecodeResult result;
    uint32_t crc{ 0 };
    while ( ( result.decodedBytes < maxBytes ) && !block->eos() ) {
        const auto headerError = block->readHeader( bitReader );
        if ( headerError == Error::END_OF_FILE ) {
            break;
        }
        if ( headerError != Error::NONE ) {
            throw std::logic_error( "Failed to read deflate block header: " + toString( headerError ) );
        }
        ++result.blockCount;

        while ( !block->eob() && ( result.decodedBytes < maxBytes ) ) {
            const auto [view, error] = block->read( bitReader, maxBytes - result.decodedBytes );
            if ( error != Error::NONE ) {
                throw std::logic_error( "Failed to decode deflate block: " + toString( error ) );
            }
            if ( computeChecksum ) {
                crc = crc32OfView( crc, view.data, view.dataWithMarkers );
            }
            result.decodedBytes += view.size();
        }
    }

    result.crc32 = crc;
    result.backreferenceCount = block->backreferences().size();
    result.containsMarkers = block->m_containsMarkerBytes;
    return result;
}


[[nodiscard]] DecodeResult
decodeWithGzipReader( const std::vector<uint8_t>& compressed,
                      const bool                  computeChecksum )
{
    GzipReader reader( std::make_unique<BufferViewFileReader>( compressed.data(), compressed.size() ) );
    reader.setCRC32Enabled( false );

    std::vector<uint8_t> outputBuffer( 1U << 20U );
    DecodeResult result;
    uint32_t crc{ 0 };
    while ( true ) {
        const auto nBytesRead = reader.read( -1,
                                             reinterpret_cast<char*>( outputBuffer.data() ),
                                             outputBuffer.size(),
                                             StoppingPoint::END_OF_BLOCK_HEADER );
        if ( ( nBytesRead == 0 ) && reader.eof() ) {
            break;
        }
        if ( computeChecksum ) {
            crc = updateCRC32( crc, reinterpret_cast<const char*>( outputBuffer.data() ), nBytesRead );
        }
        result.decodedBytes += nBytesRead;
    }
    result.crc32 = crc;
    if ( const auto block = reader.currentDeflateBlock() ) {
        result.backreferenceCount = block->backreferences().size();
        result.containsMarkers = block->m_containsMarkerBytes;
    }
    return result;
}


struct Dataset
{
    std::string name;
    std::vector<std::byte> data;
    std::vector<uint8_t> deflateStream;
    std::optional<std::vector<uint8_t> > gzipStream;
    VectorView<uint8_t> window;
};


[[nodiscard]] Dataset
createDataset( const std::string& name,
               const size_t       size )
{
    Dataset dataset;
    dataset.name = name;
    dataset.data.resize( size );
    if ( name == "base64" ) {
        fillWithRandomBase64Base64Data( dataset.data );
        dataset.deflateStream = compressWithZlib( dataset.data, CompressionStrategy::HUFFMAN_ONLY, {},
                                                  ContainerFormat::DEFLATE );
        dataset.gzipStream = compressWithZlib( dataset.data, CompressionStrategy::HUFFMAN_ONLY );
    } else if ( name == "backrefs" ) {
        fillWithRandomBackreferenceData( dataset.data );
        dataset.deflateStream = compressWithZlib( dataset.data, CompressionStrategy::DEFAULT, {},
                                                  ContainerFormat::DEFLATE );
        dataset.gzipStream = compressWithZlib( dataset.data, CompressionStrategy::DEFAULT );
    } else {
        throw std::invalid_argument( "Unknown dataset: " + name );
    }

    /* A real 32 KiB window as produced by a seek point. A valid stream never references before its start,
     * so this only selects the uint8 window path; the validation crc proves that it was not needed. */
    const auto windowSize = std::min<size_t>( deflate::MAX_WINDOW_SIZE, dataset.data.size() );
    const auto* windowBegin =
        reinterpret_cast<const uint8_t*>( dataset.data.data() + dataset.data.size() - windowSize );
    dataset.window = VectorView<uint8_t>( windowBegin, windowSize );
    return dataset;
}


[[nodiscard]] std::pair<DecodeResult, double>
runOnce( const Dataset&   dataset,
         const DecodeMode mode,
         const size_t     maxBytes,
         const bool       computeChecksum )
{
    const auto start = now();
    DecodeResult result;
    switch ( mode )
    {
    case DecodeMode::NO_WINDOW_FIRST:
    case DecodeMode::NO_WINDOW_CHUNK:
    case DecodeMode::NO_WINDOW_FULL:
        result = decodeWithBlock( dataset.deflateStream, mode, maxBytes, {}, computeChecksum );
        break;
    case DecodeMode::WINDOW_FULL:
        result = decodeWithBlock( dataset.deflateStream, mode, maxBytes, dataset.window, computeChecksum );
        break;
    case DecodeMode::USAGE_TRACKING:
        result = decodeWithBlock( dataset.deflateStream, mode, maxBytes, {}, computeChecksum );
        break;
    case DecodeMode::GZIP_READER:
        if ( !dataset.gzipStream ) {
            throw std::logic_error( "Missing gzip stream!" );
        }
        result = decodeWithGzipReader( *dataset.gzipStream, computeChecksum );
        break;
    }
    return { result, duration( start ) };
}
}  // namespace


int
main( int    argc,
      char** argv )
{
    using namespace rapidgzip;

    std::vector<std::string> datasetNames{ "base64", "backrefs" };
    std::vector<DecodeMode> modes{
        DecodeMode::NO_WINDOW_FIRST,
        DecodeMode::NO_WINDOW_CHUNK,
        DecodeMode::NO_WINDOW_FULL,
        DecodeMode::WINDOW_FULL,
        DecodeMode::USAGE_TRACKING,
        DecodeMode::GZIP_READER,
    };
    size_t size{ 64_Mi };
    size_t iterations{ 5 };
    size_t warmupIterations{ 1 };

    for ( int i = 1; i < argc; ++i ) {
        const std::string argument = argv[i];
        if ( ( argument == "--data" ) && ( i + 1 < argc ) ) {
            datasetNames = { argv[++i] };
        } else if ( ( argument == "--mode" ) && ( i + 1 < argc ) ) {
            const std::string modeName = argv[++i];
            modes.clear();
            for ( const auto mode : { DecodeMode::NO_WINDOW_FIRST, DecodeMode::NO_WINDOW_CHUNK,
                                      DecodeMode::NO_WINDOW_FULL, DecodeMode::WINDOW_FULL,
                                      DecodeMode::USAGE_TRACKING, DecodeMode::GZIP_READER } ) {
                if ( modeName == toString( mode ) ) {
                    modes.push_back( mode );
                }
            }
            if ( modes.empty() ) {
                throw std::invalid_argument( "Unknown mode: " + modeName );
            }
        } else if ( ( argument == "--size" ) && ( i + 1 < argc ) ) {
            size = parseByteSize( argv[++i] );
        } else if ( ( argument == "--iterations" ) && ( i + 1 < argc ) ) {
            iterations = std::stoull( argv[++i] );
        } else if ( ( argument == "--warmup" ) && ( i + 1 < argc ) ) {
            warmupIterations = std::stoull( argv[++i] );
        } else {
            std::cerr << "Unknown argument: " << argument << "\n";
            return 2;
        }
    }

    for ( const auto& datasetName : datasetNames ) {
        const auto dataset = createDataset( datasetName, size );
        std::cout << "# dataset=" << dataset.name
                  << " decoded=" << dataset.data.size()
                  << " deflate=" << dataset.deflateStream.size()
                  << " gzip=" << ( dataset.gzipStream ? dataset.gzipStream->size() : 0 ) << "\n";

        for ( const auto mode : modes ) {
            const auto maxBytes = ( mode == DecodeMode::NO_WINDOW_FIRST || mode == DecodeMode::USAGE_TRACKING )
                                  ? deflate::MAX_WINDOW_SIZE
                                  : ( mode == DecodeMode::NO_WINDOW_CHUNK
                                      ? FIRST_CHUNK_SIZE
                                      : std::numeric_limits<size_t>::max() );

            /* Untimed validation run: it produces the crc and the byte count reported below. Timed runs decode
             * into the Block window and do not checksum, so they measure the inflate loop only. */
            const auto [expected, validationDuration] = runOnce( dataset, mode, maxBytes, true );

            for ( size_t i = 0; i < warmupIterations; ++i ) {
                (void)runOnce( dataset, mode, maxBytes, false );
            }

            for ( size_t i = 0; i < iterations; ++i ) {
                const auto [result, seconds] = runOnce( dataset, mode, maxBytes, false );
                if ( ( result.decodedBytes != expected.decodedBytes )
                     || ( result.blockCount != expected.blockCount ) ) {
                    throw std::logic_error( "Timed run produced a different result!" );
                }
                std::cout << "dataset=" << dataset.name
                          << " mode=" << toString( mode )
                          << " iteration=" << i
                          << " bytes=" << result.decodedBytes
                          << " crc=0x" << std::hex << std::setw( 8 ) << std::setfill( '0' ) << expected.crc32
                          << std::dec << std::setfill( ' ' )
                          << " seconds=" << seconds
                          << " MBps=" << ( static_cast<double>( result.decodedBytes ) / 1e6 / seconds )
                          << " blocks=" << result.blockCount
                          << " backrefs=" << result.backreferenceCount
                          << " markers=" << result.containsMarkers
                          << "\n";
            }
            std::cout << "# validation-seconds=" << validationDuration << "\n";
        }
    }

    return 0;
}
