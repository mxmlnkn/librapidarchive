#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <core/DataGenerators.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <filereader/Standard.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/Bzip2Compressor.hpp>
#include <indexed_bzip2/ParallelBZ2Reader.hpp>


using namespace indexed_bzip2;
using namespace rapidgzip;


namespace
{
[[nodiscard]] std::vector<uint8_t>
decode( const std::vector<uint8_t>& compressed,
        const size_t                parallelism )
{
    std::unique_ptr<BZ2ReaderInterface> reader;
    if ( parallelism == 1 ) {
        reader = std::make_unique<BZ2Reader>(
            std::make_unique<BufferViewFileReader>( compressed ) );
    } else {
        reader = std::make_unique<ParallelBZ2Reader>(
            std::make_unique<BufferViewFileReader>( compressed ), parallelism );
    }

    std::vector<uint8_t> result;
    std::array<char, 8192> buffer{};
    while ( true ) {
        const auto nBytesRead = reader->read( -1, buffer.data(), buffer.size() );
        if ( nBytesRead == 0 ) {
            break;
        }
        const auto* const bytes = reinterpret_cast<const uint8_t*>( buffer.data() );
        result.insert( result.end(), bytes, bytes + nBytesRead );
    }
    return result;
}


void
testStreams()
{
    const auto text = std::vector<uint8_t>( 100000, static_cast<uint8_t>( 'A' ) );
    auto random = std::vector<uint8_t>( 200000 );
    fillWithRandomData( random.data(), random.size() );

    for ( const auto level : { 1, 9 } ) {
        const auto textStream = compressBzip2Stream( text.data(), text.size(), level );
        REQUIRE( decode( textStream, 1 ) == text );
        REQUIRE( decode( textStream, 2 ) == text );

        const auto randomStream = compressBzip2Stream( random.data(), random.size(), level );
        REQUIRE( decode( randomStream, 1 ) == random );
        REQUIRE( decode( randomStream, 2 ) == random );

        auto concatenated = textStream;
        concatenated.insert( concatenated.end(), randomStream.begin(), randomStream.end() );
        auto expected = text;
        expected.insert( expected.end(), random.begin(), random.end() );
        REQUIRE( decode( concatenated, 1 ) == expected );
        REQUIRE( decode( concatenated, 2 ) == expected );

        const auto emptyStream = compressBzip2Stream( nullptr, 0, level );
        REQUIRE( !emptyStream.empty() );
        REQUIRE( decode( emptyStream, 1 ).empty() );
        REQUIRE( decode( emptyStream, 2 ).empty() );
    }
}


void
testParallelFileCompression( const std::vector<uint8_t>& input,
                             const int                   level,
                             const size_t                parallelism )
{
    const auto tmpFolder = createTemporaryDirectory( "testBzip2Compressor" );
    const auto inputPath = tmpFolder.path() / "input";
    const auto outputPath = tmpFolder.path() / "output.bz2";

    {
        std::ofstream output( inputPath, std::ios::binary );
        output.write( reinterpret_cast<const char*>( input.data() ), static_cast<std::streamsize>( input.size() ) );
    }

    auto inputFile = std::make_unique<StandardFileReader>( inputPath.string() );
    auto outputFile = std::make_unique<OutputFile>( outputPath.string() );
    const auto statistics = compressBzip2Parallel( *inputFile, outputFile->fd(), level, parallelism );
    outputFile.reset();

    REQUIRE_EQUAL( statistics.uncompressedBytes, input.size() );
    REQUIRE( statistics.compressedBytes > 0 );
    const auto compressed = readFile<std::vector<uint8_t> >( outputPath );
    REQUIRE_EQUAL( compressed.size(), statistics.compressedBytes );
    REQUIRE( decode( compressed, 1 ) == input );
    REQUIRE( decode( compressed, 2 ) == input );
}
}  // namespace


int
main()
{
    testStreams();

    const auto text = std::vector<uint8_t>( 300000, static_cast<uint8_t>( 'B' ) );
    auto random = std::vector<uint8_t>( 400000 );
    fillWithRandomData( random.data(), random.size() );
    for ( const auto level : { 1, 9 } ) {
        for ( const auto parallelism : { size_t( 1 ), size_t( 2 ), size_t( 3 ) } ) {
            testParallelFileCompression( text, level, parallelism );
            testParallelFileCompression( random, level, parallelism );
        }
    }
    testParallelFileCompression( {}, 9, 2 );

    std::cout << "Tests successful: " << gnTests << " / " << ( gnTests + gnTestErrors ) << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
