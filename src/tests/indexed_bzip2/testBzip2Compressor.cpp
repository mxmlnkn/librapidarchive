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
testScanChunkOffsets()
{
    const auto text = std::vector<uint8_t>( 100000, static_cast<uint8_t>( 'A' ) );
    const auto singleBlock = compressBzip2Stream( text.data(), text.size(), 9 );
    const auto singleScan = scanBzip2ChunkOffsets( singleBlock );
    REQUIRE( singleScan.firstBlockBit == 32U );
    REQUIRE( singleScan.endOfStreamBit.has_value() );
    REQUIRE( singleScan.ambiguousBoundaryBits.empty() );

    const auto emptyStream = compressBzip2Stream( nullptr, 0, 9 );
    const auto emptyScan = scanBzip2ChunkOffsets( emptyStream );
    REQUIRE( !emptyScan.firstBlockBit );
    REQUIRE( emptyScan.endOfStreamBit == 32U );
    REQUIRE( emptyScan.ambiguousBoundaryBits.empty() );

    /* 500 kB at level 1 exceeds the 100 kB block size, so libbz2 splits it into multiple blocks. Their
     * decoded sizes are not stored in the stream and must therefore be reported as ambiguous. */
    auto random = std::vector<uint8_t>( 500000 );
    fillWithRandomData( random.data(), random.size() );
    const auto multiBlock = compressBzip2Stream( random.data(), random.size(), 1 );
    const auto multiScan = scanBzip2ChunkOffsets( multiBlock );
    REQUIRE( multiScan.firstBlockBit == 32U );
    REQUIRE( multiScan.endOfStreamBit.has_value() );
    REQUIRE( !multiScan.ambiguousBoundaryBits.empty() );

    /* A concatenated stream boundary is an EOS followed by another stream header, not the final EOS. */
    auto concatenated = singleBlock;
    concatenated.insert( concatenated.end(), singleBlock.begin(), singleBlock.end() );
    const auto concatenatedScan = scanBzip2ChunkOffsets( concatenated );
    REQUIRE( concatenatedScan.firstBlockBit == 32U );
    REQUIRE( concatenatedScan.endOfStreamBit.has_value() );
    REQUIRE( !concatenatedScan.ambiguousBoundaryBits.empty() );
    REQUIRE( concatenatedScan.endOfStreamBit > singleBlock.size() * 8U );
}


void
testWorstCaseRleExpansionWithIndex()
{
    /* Runs of exactly four bytes expand by 5/4 in RLE1. With the old 80% cap, an 80 kB chunk at level 1
     * expanded to the full libbz2 block size and produced two blocks, so the in-memory offset scan rejected
     * a valid chunk. */
    std::vector<uint8_t> input( 80000 );
    for ( size_t i = 0; i < input.size(); ++i ) {
        input[i] = static_cast<uint8_t>( ( i / 4U ) % 256U );
    }

    const auto tmpFolder = createTemporaryDirectory( "testBzip2Compressor" );
    const auto inputPath = tmpFolder.path() / "input";
    const auto outputPath = tmpFolder.path() / "output.bz2";
    {
        std::ofstream output( inputPath, std::ios::binary );
        output.write( reinterpret_cast<const char*>( input.data() ), static_cast<std::streamsize>( input.size() ) );
    }

    std::map<size_t, size_t> offsets;
    auto inputFile = std::make_unique<StandardFileReader>( inputPath.string() );
    auto outputFile = std::make_unique<OutputFile>( outputPath.string() );
    const auto statistics = compressBzip2Parallel(
        *inputFile, outputFile->fd(), /* level */ 1, /* parallelism */ 2,
        [&offsets] ( const size_t compressedOffset, const size_t decodedOffset ) {
            offsets.emplace( compressedOffset, decodedOffset );
        } );
    outputFile.reset();
    REQUIRE_EQUAL( statistics.uncompressedBytes, input.size() );

    const auto compressed = readFile<std::vector<uint8_t> >( outputPath );
    REQUIRE( decode( compressed, 1 ) == input );

    std::string index;
    writeBzip2Index( offsets, [&index] ( const void* data, const size_t size ) {
                         const auto* const bytes = reinterpret_cast<const char*>( data );
                         index.append( bytes, size );
                     } );

    const std::vector<char> indexData( index.begin(), index.end() );
    ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), 2 );
    reader.importIndex( std::make_unique<BufferViewFileReader>( indexData ) );
    std::vector<uint8_t> decoded;
    std::array<char, 8192> buffer{};
    while ( true ) {
        const auto nBytesRead = reader.read( -1, buffer.data(), buffer.size() );
        if ( nBytesRead == 0 ) {
            break;
        }
        const auto* const bytes = reinterpret_cast<const uint8_t*>( buffer.data() );
        decoded.insert( decoded.end(), bytes, bytes + nBytesRead );
    }
    REQUIRE( decoded == input );
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
    testScanChunkOffsets();
    testWorstCaseRleExpansionWithIndex();

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
