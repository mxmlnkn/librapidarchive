#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <core/common.hpp>
#include <core/DataGenerators.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <filereader/Standard.hpp>
#include <indexed_bzip2/IndexFileFormat.hpp>
#include <indexed_bzip2/ParallelBZ2Reader.hpp>

#include "Bzip2ReaderTests.hpp"
#include "Bzip2TestHelpers.hpp"


using namespace rapidgzip;
using namespace indexed_bzip2;
using namespace bzip2_test;


struct ParallelReaderPolicy
{
    using Reader = ParallelBZ2Reader;

    static constexpr bool SUPPORTS_NON_SEEKABLE{ false };

    static std::unique_ptr<Reader>
    make( std::unique_ptr<FileReader> file,
          const size_t                parallelization )
    {
        return std::make_unique<Reader>( std::move( file ), parallelization );
    }

    static void
    joinThreads( Reader& reader )
    {
        reader.joinThreads();
    }
};


void
testMalformedBzip2( const std::filesystem::path& dataFolder,
                    const std::string&           fileName,
                    const std::string&           expectedMessageFragment = {} )
{
    /* The inputs are malformed and used to trip Debug assertions or leak std::bad_optional_access. They must be
     * rejected with a descriptive exception instead. */
    ParallelBZ2Reader reader(
        std::make_unique<StandardFileReader>( ( dataFolder / fileName ).string() ),
        /* parallelization */ 2 );
    std::vector<char> buffer( 4_Ki );
    std::string message;
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( !message.empty() );
    REQUIRE( message.find( "bad optional access" ) == std::string::npos );
    if ( !expectedMessageFragment.empty() ) {
        REQUIRE( message.find( expectedMessageFragment ) != std::string::npos );
    }
}


void
testJoinThreadsMidStream( const std::string& decodedTestFilePath,
                          const std::string& encodedTestFilePath )
{
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    for ( const auto joinAfterReads : { 0U, 1U, 2U, 5U } ) {
        ParallelBZ2Reader reader( std::make_unique<StandardFileReader>( encodedTestFilePath ),
                                  /* parallelization */ 4 );

        std::vector<char> decoded;
        std::vector<char> buffer( 4_Ki );
        for ( size_t readCount = 0; ; ++readCount ) {
            const auto nBytesRead = reader.read( buffer.data(), buffer.size() );
            if ( nBytesRead == 0 ) {
                break;
            }
            decoded.insert( decoded.end(), buffer.data(), buffer.data() + nBytesRead );
            if ( readCount == joinAfterReads ) {
                reader.joinThreads();
            }
        }

        REQUIRE_EQUAL( decoded.size(), expected.size() );
        REQUIRE( ( decoded.size() == expected.size() )
                 && std::equal( decoded.begin(), decoded.end(), expected.begin() ) );
    }
}


void
testExactDecodedSize( const std::filesystem::path& tmpFolder )
{
    /* Runs at and around the RLE1 boundaries. A run of 4..255 identical bytes is stored as the four
     * bytes followed by a count byte and decodes to 4 + count bytes. Shorter runs stay literals, and
     * runs of 256 or more are split. The exact-size scan must agree with decodeBlock for all of them,
     * otherwise the preallocated output buffer is either too small or exposes uninitialized bytes. */
    std::vector<char> expected;
    const std::vector<size_t> runLengths{ 1, 2, 3, 4, 5, 254, 255, 256, 259, 260, 516, 517 };
    for ( size_t repeat = 0; repeat < 400; ++repeat ) {
        for ( const auto runLength : runLengths ) {
            expected.insert( expected.end(), runLength, 'A' );
            expected.emplace_back( 'B' );
        }
    }

    const auto decodedPath = tmpFolder / "exact-size-decoded";
    const auto encodedPath = tmpFolder / "exact-size.bz2";
    {
        std::ofstream file( decodedPath, std::ios::binary );
        file.write( expected.data(), static_cast<std::streamsize>( expected.size() ) );
    }

    bzip2_test::compressFile( decodedPath, encodedPath );

    for ( const auto parallelization : { 1U, 2U, 8U } ) {
        ParallelBZ2Reader reader( std::make_unique<StandardFileReader>( encodedPath.string() ), parallelization );

        const auto decoded = bzip2_test::readAll( reader );

        REQUIRE_EQUAL( decoded.size(), expected.size() );
        REQUIRE( std::equal( decoded.begin(), decoded.end(), expected.begin(), expected.end() ) );
        REQUIRE( reader.size().has_value() );
        REQUIRE_EQUAL( *reader.size(), expected.size() );
    }
}


int
main( int    argc,
      char** argv )
{
    if ( argc == 0 ) {
        std::cerr << "Expected at least the launch command as the first argument!\n";
        return 1;
    }

    const std::string binaryFilePath( argv[0] );
    std::string binaryFolder = ".";
    if ( const auto lastSlash = binaryFilePath.find_last_of( '/' ); lastSlash != std::string::npos ) {
        binaryFolder = binaryFilePath.substr( 0, lastSlash );
    }
    const auto dataFolder =
        std::filesystem::path(
            findParentFolderContaining( binaryFolder, "src/tests/data/base64-256KiB.bgz" )
        ) / "src" / "tests" / "data";

    testMalformedBzip2( dataFolder, "bzip2-read-selectors-overread.bz2" );
    testMalformedBzip2( dataFolder, "bzip2-block-finder-false-positive.bz2" );
    testMalformedBzip2( dataFolder, "bzip2-invalid-huffman-code.bz2", "Invalid Huffman code" );

    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testParallelBZ2Reader" );

    testExactDecodedSize( tmpFolder.path() );

    const auto decodedTestFilePath = tmpFolder.path() / "decoded";
    createRandomTextFile( decodedTestFilePath.string(), 2_Mi );

    const auto encodedTestFilePath = tmpFolder.path() / "encoded-sample.bz2";
    bzip2_test::compressFile( decodedTestFilePath, encodedTestFilePath );

    try
    {
        for ( const auto parallelization : { 1U, 2U } ) {
            testSimpleOpenAndClose<ParallelReaderPolicy>( encodedTestFilePath.string(), parallelization );
            testNonSeekableEmptyInput<ParallelReaderPolicy>( parallelization );
            testEmptyAndTruncatedInputs<ParallelReaderPolicy>( encodedTestFilePath.string(), parallelization );
            testRunLengthOverflow<ParallelReaderPolicy>( parallelization );
            testOversizedCodeLength<ParallelReaderPolicy>( parallelization );
            testInvalidBlockOffsetsLeaveReaderUsable<ParallelReaderPolicy>( encodedTestFilePath.string(),
                                                                            parallelization );
            testSingleBlockCRCs<ParallelReaderPolicy>( parallelization );
            testMultiBlockCRCVerification<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
            testImportedIndexVerifiesStreamCRC<ParallelReaderPolicy>( parallelization );
            testImportedOffsetsVerifyStreamCRC<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
            testImportedDecodedSizesAreValidated<ParallelReaderPolicy>(
                encodedTestFilePath.string(),
                parallelization ); testStreamCRCVerificationAfterSeekToStart<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
            testConcatenatedStreamCRCVerification<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
            testRejectsIndexForFirstConcatenatedStream<ParallelReaderPolicy>(
                encodedTestFilePath.string(), parallelization );
            testJoinThreadsMidStream( decodedTestFilePath.string(), encodedTestFilePath.string() );
            testDecodingBz2ForFirstTime<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
            testIndexImportExport<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
            testIncompleteIndexIsRejected<ParallelReaderPolicy>( encodedTestFilePath.string(), parallelization );
            testConcatenatedStreams<ParallelReaderPolicy>(
                decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
        }
    } catch ( const std::exception& exception ) {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
