/**
 * @file Focused end-to-end decode benchmark for rapidgzip.
 *
 * Unlike benchmarkGzip, this measures a single configuration so that performance changes to the
 * chunk pipeline can be compared with alternating A/B runs. It reports one line per iteration and
 * leaves median/range computation to the caller.
 *
 * cmake --build . --target benchmarkGzipDecode &&
 * taskset -c 6 src/benchmarks/benchmarkGzipDecode <file.gz> [-p N] [-c BYTES] [-i N] [--no-crc] [--sequential]
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <core/common.hpp>
#include <core/FileRanges.hpp>
#include <filereader/Standard.hpp>
#include <rapidgzip/gzip/GzipReader.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>


namespace rapidgzip
{
size_t
decompressSequential( const std::string& fileName,
                      const bool         verifyChecksum )
{
    GzipReader reader( std::make_unique<StandardFileReader>( fileName ) );
    reader.setCRC32Enabled( verifyChecksum );

    std::vector<uint8_t> outputBuffer( 16_Mi );
    size_t totalDecodedBytes = 0;
    while ( true ) {
        const auto nBytesRead = reader.read(
            -1, reinterpret_cast<char*>( outputBuffer.data() ), outputBuffer.size() );
        if ( ( nBytesRead == 0 ) && reader.eof() ) {
            break;
        }
        totalDecodedBytes += nBytesRead;
    }
    return totalDecodedBytes;
}


struct Arguments
{
    std::string fileName{};
    size_t parallelization{ 1 };
    uint64_t chunkSize{ 4_Mi };
    size_t iterations{ 3 };
    bool verifyChecksum{ true };
    bool sequential{ false };
    bool statistics{ false };
    bool windowSparsity{ true };
    bool windowCompression{ true };
    bool keepIndex{ true };
    bool discardOutput{ false };
};


size_t
decompressParallel( const Arguments& arguments )
{
    ParallelGzipReader reader( std::make_unique<StandardFileReader>( arguments.fileName ),
                               arguments.parallelization, arguments.chunkSize );
    reader.setCRC32Enabled( arguments.verifyChecksum );
    reader.setWindowSparsity( arguments.windowSparsity );
    if ( !arguments.windowCompression ) {
        reader.setWindowCompressionType( CompressionType::NONE );
    }
    reader.setKeepIndex( arguments.keepIndex );
    reader.setStatisticsEnabled( arguments.statistics );
    reader.setShowProfileOnDestruction( arguments.statistics );

    std::vector<uint8_t> outputBuffer( 16_Mi );
    size_t totalDecodedBytes = 0;
    while ( true ) {
        const auto nBytesRead =
            arguments.discardOutput
            ? reader.read( [] ( const auto&, size_t, size_t ) {}, outputBuffer.size() )
            : reader.read( -1, reinterpret_cast<char*>( outputBuffer.data() ), outputBuffer.size() );
        if ( ( nBytesRead == 0 ) && reader.eof() ) {
            break;
        }
        totalDecodedBytes += nBytesRead;
    }
    return totalDecodedBytes;
}
}  // namespace rapidgzip


int
main( int    argc,
      char** argv )
{
    if ( argc < 2 ) {
        std::cerr << "Usage: " << argv[0] << " <file.gz> [-p N] [-c BYTES] [-i N] [--no-crc] [--sequential]"
                  << " [--stats] [--no-sparsity] [--no-window-compression] [--no-keep-index]"
                  << " [--discard-output]\n";
        return 1;
    }

    using namespace rapidgzip;

    Arguments arguments;
    arguments.fileName = argv[1];

    for ( int i = 2; i < argc; ++i ) {
        const std::string argument = argv[i];
        if ( ( argument == "-p" ) && ( i + 1 < argc ) ) {
            arguments.parallelization = std::stoull( argv[++i] );
        } else if ( ( argument == "-c" ) && ( i + 1 < argc ) ) {
            arguments.chunkSize = parseByteSize( argv[++i] );
        } else if ( ( argument == "-i" ) && ( i + 1 < argc ) ) {
            arguments.iterations = std::stoull( argv[++i] );
        } else if ( argument == "--no-crc" ) {
            arguments.verifyChecksum = false;
        } else if ( argument == "--stats" ) {
            arguments.statistics = true;
        } else if ( argument == "--no-sparsity" ) {
            arguments.windowSparsity = false;
        } else if ( argument == "--no-window-compression" ) {
            arguments.windowCompression = false;
        } else if ( argument == "--no-keep-index" ) {
            arguments.keepIndex = false;
        } else if ( argument == "--discard-output" ) {
            arguments.discardOutput = true;
        } else if ( argument == "--sequential" ) {
            arguments.sequential = true;
        } else {
            std::cerr << "Unknown argument: " << argument << "\n";
            return 2;
        }
    }

    /* Warm up caches, page mappings, and the CPU frequency governor. */
    if ( arguments.sequential ) {
        decompressSequential( arguments.fileName, arguments.verifyChecksum );
    } else {
        decompressParallel( arguments );
    }

    for ( size_t iteration = 0; iteration < arguments.iterations; ++iteration ) {
        const auto start = std::chrono::steady_clock::now();
        const auto nBytesDecoded = arguments.sequential
                                   ? decompressSequential( arguments.fileName, arguments.verifyChecksum )
                                   : decompressParallel( arguments );
        const auto stop = std::chrono::steady_clock::now();
        const auto seconds = std::chrono::duration<double>( stop - start ).count();
        std::cout << "bytes=" << nBytesDecoded
                  << " iteration=" << iteration
                  << " seconds=" << seconds
                  << " MBps=" << ( static_cast<double>( nBytesDecoded ) / 1e6 / seconds )
                  << "\n";
    }

    return 0;
}
