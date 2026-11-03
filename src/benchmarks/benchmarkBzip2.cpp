/**
 * @file Benchmarks the sequential and parallel bzip2 decoders on already decompressed input files.
 *
 * The compressed input is kept in memory so that the measurement is not dominated by file I/O.
 * Two modes are provided: "discard" decodes without copying the output into a caller buffer,
 * which isolates the decoder, and "copy" additionally writes each chunk into a reused output buffer,
 * which is closer to a normal caller. Both modes return the number of decoded bytes so that a
 * decoding regression that silently drops data is visible.
 */

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <core/FileUtils.hpp>
#include <filereader/BufferView.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/ParallelBZ2Reader.hpp>


namespace
{
enum class Mode
{
    DISCARD,
    COPY,
};


struct Arguments
{
    std::vector<std::string> files;
    size_t iterations{ 5 };
    size_t parallelization{ 1 };
    Mode mode{ Mode::DISCARD };
};


[[nodiscard]] Arguments
parseArguments( int    argc,
                char** argv )
{
    Arguments arguments;
    for ( int i = 1; i < argc; ++i ) {
        const std::string argument = argv[i];
        if ( ( argument == "--iterations" ) && ( i + 1 < argc ) ) {
            arguments.iterations = std::stoul( argv[++i] );
        } else if ( ( argument == "--parallel" ) && ( i + 1 < argc ) ) {
            arguments.parallelization = std::stoul( argv[++i] );
        } else if ( ( argument == "--mode" ) && ( i + 1 < argc ) ) {
            const std::string mode = argv[++i];
            if ( mode == "discard" ) {
                arguments.mode = Mode::DISCARD;
            } else if ( mode == "copy" ) {
                arguments.mode = Mode::COPY;
            } else {
                throw std::invalid_argument( "Invalid mode: " + mode );
            }
        } else if ( argument == "--help" ) {
            std::cout << "Usage: " << argv[0]
                      << " [--iterations N] [--parallel N] [--mode discard|copy] FILE.bz2...\n";
            std::exit( 0 );
        } else {
            arguments.files.emplace_back( argument );
        }
    }

    if ( arguments.files.empty() ) {
        throw std::invalid_argument( "Please specify at least one bzip2-compressed test file!" );
    }
    return arguments;
}


template<typename Reader>
[[nodiscard]] size_t
readAll( Reader&          reader,
         const Arguments& arguments )
{
    if ( arguments.mode == Mode::DISCARD ) {
        return reader.read( [] ( const void*, uint64_t ) {}, std::numeric_limits<size_t>::max() );
    }

    std::vector<char> outputBuffer( 1UL << 20U );
    size_t totalDecodedBytes = 0;
    while ( true ) {
        const auto nBytesDecoded = reader.read( outputBuffer.data(), outputBuffer.size() );
        totalDecodedBytes += nBytesDecoded;
        if ( ( nBytesDecoded == 0 ) || reader.eof() ) {
            return totalDecodedBytes;
        }
    }
}


template<typename Reader>
[[nodiscard]] size_t
decodeOnce( const std::vector<std::byte>& compressedData,
            const Arguments&              arguments )
{
    if constexpr ( std::is_same_v<Reader, indexed_bzip2::ParallelBZ2Reader> ) {
        Reader reader( std::make_unique<rapidgzip::BufferViewFileReader>( compressedData ),
                       arguments.parallelization );
        return readAll( reader, arguments );
    } else {
        Reader reader( std::make_unique<rapidgzip::BufferViewFileReader>( compressedData ) );
        return readAll( reader, arguments );
    }
}


template<typename Reader>
void
benchmarkFile( const std::string& fileName,
               const Arguments&   arguments )
{
    const auto compressedData = rapidgzip::readFile<std::vector<std::byte> >( fileName );

    /* Warm up caches, the file mapping, and the frequency scaling before the measured runs. */
    const auto expectedSize = decodeOnce<Reader>( compressedData, arguments );

    std::vector<double> durations( arguments.iterations );
    size_t decodedSize = 0;
    for ( auto& duration : durations ) {
        const auto t0 = std::chrono::steady_clock::now();
        decodedSize = decodeOnce<Reader>( compressedData, arguments );
        duration = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
    }

    if ( decodedSize != expectedSize ) {
        throw std::logic_error( "Decoder returned a varying number of bytes!" );
    }

    std::vector<double> bandwidths( durations.size() );
    std::transform( durations.begin(), durations.end(), bandwidths.begin(),
                    [&decodedSize] ( const double duration ) {
                        return static_cast<double>( decodedSize ) / 1e6 / duration;
                    } );
    std::vector<double> sortedBandwidths = bandwidths;
    std::sort( sortedBandwidths.begin(), sortedBandwidths.end() );

    std::cout << fileName << " (" << compressedData.size() << " B compressed, " << decodedSize
              << " B decoded, parallelization " << arguments.parallelization << ", "
              << ( arguments.mode == Mode::COPY ? "copy" : "discard" ) << "):\n";
    std::cout << "    MB/s: [";
    for ( size_t i = 0; i < bandwidths.size(); ++i ) {
        std::cout << ( i == 0 ? "" : ", " ) << bandwidths[i];
    }
    std::cout << "], median " << sortedBandwidths[sortedBandwidths.size() / 2]
              << ", range " << sortedBandwidths.front() << " .. " << sortedBandwidths.back() << "\n";
}
}  // namespace


int
main( int    argc,
      char** argv )
{
    try
    {
        const auto arguments = parseArguments( argc, argv );
        for ( const auto& fileName : arguments.files ) {
            if ( arguments.parallelization <= 1 ) {
                benchmarkFile<indexed_bzip2::BZ2Reader>( fileName, arguments );
            } else {
                benchmarkFile<indexed_bzip2::ParallelBZ2Reader>( fileName, arguments );
            }
        }
    } catch ( const std::exception& exception ) {
        std::cerr << "Failed to benchmark: " << exception.what() << "\n";
        return 1;
    }
    return 0;
}
