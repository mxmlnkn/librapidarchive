/**
 * @file Focused end-to-end decode benchmark for rapidgzip.
 *
 * Unlike benchmarkGzip, this measures a single configuration so that performance changes to the
 * chunk pipeline can be compared with alternating A/B runs. It reports one line per iteration and
 * leaves median/range computation to the caller.
 *
 * Build:
 *   cmake --build build --target benchmarkGzipDecode
 *
 * Usage:
 *   taskset -c 6 ./benchmarkGzipDecode <file.gz> [-p N] [-c BYTES] [-i N] [--no-crc] [--sequential]
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
#include <filereader/Standard.hpp>
#include <rapidgzip/gzip/GzipReader.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>


namespace
{
[[nodiscard]] uint64_t
parseSize( const std::string& value )
{
    size_t end{ 0 };
    const auto number = std::stoull( value, &end );
    if ( end == value.size() ) {
        return number;
    }
    auto suffix = value.substr( end );
    if ( !suffix.empty() && ( suffix.back() == 'i' ) ) {
        suffix.pop_back();
    }
    if ( ( suffix == "K" ) || ( suffix == "KB" ) ) {
        return number * 1024U;
    }
    if ( ( suffix == "M" ) || ( suffix == "MB" ) ) {
        return number * 1024U * 1024U;
    }
    if ( ( suffix == "G" ) || ( suffix == "GB" ) ) {
        return number * 1024U * 1024U * 1024U;
    }
    throw std::invalid_argument( "Cannot parse size: " + value );
}


[[nodiscard]] size_t
decompressSequential( const std::string& fileName,
                      const bool         verifyChecksum )
{
    rapidgzip::GzipReader reader( std::make_unique<rapidgzip::StandardFileReader>( fileName ) );
    reader.setCRC32Enabled( verifyChecksum );

    std::vector<uint8_t> outputBuffer( 16U * 1024U * 1024U );
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


[[nodiscard]] size_t
decompressParallel( const std::string& fileName,
                    const size_t       parallelization,
                    const uint64_t     chunkSize,
                    const bool         verifyChecksum,
                    const bool         statistics,
                    const bool         windowSparsity,
                    const bool         windowCompression,
                    const bool         keepIndex,
                    const bool         discardOutput )
{
    rapidgzip::ParallelGzipReader reader(
        std::make_unique<rapidgzip::StandardFileReader>( fileName ), parallelization, chunkSize );
    reader.setCRC32Enabled( verifyChecksum );
    if ( !windowSparsity ) {
        reader.setWindowSparsity( false );
    }
    if ( !windowCompression ) {
        reader.setWindowCompressionType( rapidgzip::CompressionType::NONE );
    }
    if ( !keepIndex ) {
        reader.setKeepIndex( false );
    }
    if ( statistics ) {
        reader.setStatisticsEnabled( true );
        reader.setShowProfileOnDestruction( true );
    }

    std::vector<uint8_t> outputBuffer( 16U * 1024U * 1024U );
    size_t totalDecodedBytes = 0;
    while ( true ) {
        const auto nBytesRead = discardOutput
                                ? reader.read( [] ( const auto&, size_t, size_t ) {}, outputBuffer.size() )
                                : reader.read( -1, reinterpret_cast<char*>( outputBuffer.data() ),
                                               outputBuffer.size() );
        if ( ( nBytesRead == 0 ) && reader.eof() ) {
            break;
        }
        totalDecodedBytes += nBytesRead;
    }
    return totalDecodedBytes;
}
}  // namespace


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

    const std::string fileName = argv[1];
    size_t parallelization{ 1 };
    uint64_t chunkSize{ 4U * 1024U * 1024U };
    size_t iterations{ 3 };
    bool verifyChecksum{ true };
    bool sequential{ false };
    bool statistics{ false };
    bool windowSparsity{ true };
    bool windowCompression{ true };
    bool keepIndex{ true };
    bool discardOutput{ false };

    for ( int i = 2; i < argc; ++i ) {
        const std::string argument = argv[i];
        if ( ( argument == "-p" ) && ( i + 1 < argc ) ) {
            parallelization = std::stoull( argv[++i] );
        } else if ( ( argument == "-c" ) && ( i + 1 < argc ) ) {
            chunkSize = parseSize( argv[++i] );
        } else if ( ( argument == "-i" ) && ( i + 1 < argc ) ) {
            iterations = std::stoull( argv[++i] );
        } else if ( argument == "--no-crc" ) {
            verifyChecksum = false;
        } else if ( argument == "--stats" ) {
            statistics = true;
        } else if ( argument == "--no-sparsity" ) {
            windowSparsity = false;
        } else if ( argument == "--no-window-compression" ) {
            windowCompression = false;
        } else if ( argument == "--no-keep-index" ) {
            keepIndex = false;
        } else if ( argument == "--discard-output" ) {
            discardOutput = true;
        } else if ( argument == "--sequential" ) {
            sequential = true;
        } else {
            std::cerr << "Unknown argument: " << argument << "\n";
            return 2;
        }
    }

    /* Warm up caches, page mappings, and the CPU frequency governor. */
    if ( sequential ) {
        (void)decompressSequential( fileName, verifyChecksum );
    } else {
        (void)decompressParallel( fileName, parallelization, chunkSize, verifyChecksum,
                                  statistics, windowSparsity, windowCompression, keepIndex, discardOutput );
    }

    for ( size_t iteration = 0; iteration < iterations; ++iteration ) {
        const auto start = std::chrono::steady_clock::now();
        const auto nBytesDecoded = sequential
                                   ? decompressSequential( fileName, verifyChecksum )
                                   : decompressParallel( fileName, parallelization, chunkSize, verifyChecksum,
                                                         statistics, windowSparsity, windowCompression, keepIndex,
                                                         discardOutput );
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
