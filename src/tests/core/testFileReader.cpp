#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/FileReader.hpp>
#include <filereader/Standard.hpp>


using namespace rapidgzip;


std::string
fillFile( const std::string& fileName )
{
    std::stringstream result;

    std::ofstream outFile( fileName, std::ios_base::out | std::ios_base::binary );
    for ( int i = 0; i < 100; ++i ) {
        outFile << i;
        result << i;
    }

    return result.str();
}


void
testUniqueFilePointer( const std::string& tmpFileName,
                       const std::string& tmpFileContents )
{
    const auto file1 = throwingOpen( tmpFileName, "rb" );
    std::string readData( 2 * tmpFileContents.size(), '\0' );

    /* Read everything */
    {
        readData.assign( 2 * tmpFileContents.size(), '\0' );
        const auto nBytesRead = std::fread( readData.data(), 1 /* element size */, readData.size(), file1.get() );
        REQUIRE_EQUAL( nBytesRead, tmpFileContents.size() );
        REQUIRE( std::equal( tmpFileContents.begin(), tmpFileContents.end(), readData.begin() ) );
        REQUIRE( std::feof( file1.get() ) != 0 );
    }

    /* Read second time after seeking to start without clearing EOF */
    {
        readData.assign( 2 * tmpFileContents.size(), '\0' );
        fileSeek( file1.get(), 0, SEEK_SET );
        const auto nBytesRead = std::fread( readData.data(), 1 /* element size */, readData.size(), file1.get() );
        REQUIRE_EQUAL( nBytesRead, tmpFileContents.size() );
        REQUIRE( std::equal( tmpFileContents.begin(), tmpFileContents.end(), readData.begin() ) );
    }
}


void
testFileReader( const std::string& tmpFileContents,
                FileReader* const  fileReader )
{
    std::string readData( 2 * tmpFileContents.size(), '\0' );

    /* Read everything */
    {
        readData.assign( 2 * tmpFileContents.size(), '\0' );
        const auto nBytesRead = fileReader->read( readData.data(), readData.size() );
        REQUIRE_EQUAL( nBytesRead, std::min( tmpFileContents.size(), readData.size() ) );
        REQUIRE( std::equal( tmpFileContents.begin(), tmpFileContents.end(), readData.begin() ) );
        REQUIRE( fileReader->eof() != 0 );
        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() );
    }

    /* Read second time after seeking to start without clearing EOF */
    {
        readData.assign( 2 * tmpFileContents.size(), '\0' );

        fileReader->seekTo( 0 );
        REQUIRE_EQUAL( fileReader->tell(), size_t( 0 ) );

        const auto nBytesRead = fileReader->read( readData.data(), readData.size() );
        REQUIRE_EQUAL( nBytesRead, std::min( tmpFileContents.size(), readData.size() ) );
        REQUIRE( std::equal( tmpFileContents.begin(), tmpFileContents.end(), readData.begin() ) );

        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() );
    }

    /* Read from middle. */
    {
        readData.assign( 1, '\0' );
        fileReader->seek( -10, SEEK_END );
        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() - 10 );

        const auto nBytesRead = fileReader->read( readData.data(), readData.size() );
        REQUIRE_EQUAL( nBytesRead, std::min( tmpFileContents.size(), readData.size() ) );
        REQUIRE( std::equal( tmpFileContents.end() - 10, tmpFileContents.end() - 10 + 1, readData.begin() ) );
        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() - 9 );
    }
    {
        readData.assign( 1, '\0' );
        const auto nBytesRead = fileReader->read( readData.data(), readData.size() );
        REQUIRE_EQUAL( nBytesRead, std::min( tmpFileContents.size(), readData.size() ) );
        REQUIRE( std::equal( tmpFileContents.end() - 9, tmpFileContents.end() - 9 + 1, readData.begin() ) );
        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() - 8 );
    }

    /* Read multiple from middle. */
    {
        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() - 8 );
        fileReader->seek( -10, SEEK_CUR );
        REQUIRE_EQUAL( fileReader->tell(), tmpFileContents.size() - 18 );

        readData.assign( 5, '\0' );
        const auto nBytesRead = fileReader->read( readData.data(), readData.size() );
        REQUIRE_EQUAL( nBytesRead, std::min( tmpFileContents.size(), readData.size() ) );
        REQUIRE( std::equal( tmpFileContents.end() - 18, tmpFileContents.end() - 18 + 5, readData.begin() ) );
    }
}


void
testBufferedFileReaderSeek( const std::string& tmpFileName )
{
    constexpr size_t FILE_SIZE = 512_Ki;
    constexpr size_t BUFFER_SIZE = 64_Ki;

    std::vector<char> fileContents( FILE_SIZE );
    for ( size_t i = 0; i < fileContents.size(); ++i ) {
        fileContents[i] = static_cast<char>( i % 251 );
    }
    {
        std::ofstream outFile( tmpFileName, std::ios_base::out | std::ios_base::binary );
        outFile.write( fileContents.data(), static_cast<std::streamsize>( fileContents.size() ) );
    }

    BufferedFileReader fileReader( std::make_unique<StandardFileReader>( tmpFileName ), BUFFER_SIZE );

    const auto requireContentsAtCurrentPosition =
        [&] ( const size_t size )
        {
            const auto position = fileReader.tell();
            REQUIRE( position + size <= fileContents.size() );
            if ( position + size > fileContents.size() ) {
                return;
            }
            std::vector<char> readData( size );
            REQUIRE_EQUAL( fileReader.read( readData.data(), readData.size() ), size );
            REQUIRE( std::equal( readData.begin(), readData.end(),
                                 fileContents.begin() + static_cast<std::ptrdiff_t>( position ) ) );
        };

    /* Read past the first buffer so that the buffer start offset is not zero anymore. */
    for ( size_t i = 0; i < 200; ++i ) {
        requireContentsAtCurrentPosition( 1_Ki );
    }
    REQUIRE_EQUAL( fileReader.tell(), 200_Ki );
    REQUIRE( fileReader.tell() > BUFFER_SIZE );

    /* Seek to an absolute offset that is smaller than the buffer size and smaller than the buffer start offset. */
    fileReader.seekTo( 1000 );
    /* Previously, got: 197608 != 1000, and all tests below also returned wrong offsets! */
    REQUIRE_EQUAL( fileReader.tell(), size_t( 1000 ) );
    requireContentsAtCurrentPosition( 100 );

    /* Seek to an absolute offset larger than the buffer size but smaller than the buffer start offset. */
    fileReader.seekTo( 100_Ki );
    REQUIRE_EQUAL( fileReader.tell(), 100_Ki );
    requireContentsAtCurrentPosition( 100 );

    /* Seek forward to an offset outside of the current buffer. */
    fileReader.seek( 300_Ki, SEEK_SET );
    REQUIRE_EQUAL( fileReader.tell(), 300_Ki );
    requireContentsAtCurrentPosition( 100 );

    /* Seek relative to the end. */
    fileReader.seek( -10, SEEK_END );
    REQUIRE_EQUAL( fileReader.tell(), FILE_SIZE - 10 );
    requireContentsAtCurrentPosition( 10 );

    /* A read large enough to bypass the buffer must not desynchronize the buffer start offset from its contents. */
    fileReader.seekTo( 200_Ki );
    requireContentsAtCurrentPosition( 1 );
    requireContentsAtCurrentPosition( 300_Ki );
    REQUIRE_EQUAL( fileReader.tell(), 500_Ki + 1 );
    fileReader.seekTo( 500_Ki );
    REQUIRE_EQUAL( fileReader.tell(), 500_Ki );
    requireContentsAtCurrentPosition( 100 );
}


void
testBufferedFileReaderNullBuffer( const std::string& tmpFileName )
{
    constexpr size_t FILE_SIZE = 512_Ki;
    constexpr size_t BUFFER_SIZE = 64_Ki;

    std::vector<char> fileContents( FILE_SIZE );
    for ( size_t i = 0; i < fileContents.size(); ++i ) {
        fileContents[i] = static_cast<char>( i % 251 );
    }
    {
        std::ofstream outFile( tmpFileName, std::ios_base::out | std::ios_base::binary );
        outFile.write( fileContents.data(), static_cast<std::streamsize>( fileContents.size() ) );
    }

    BufferedFileReader fileReader( std::make_unique<StandardFileReader>( tmpFileName ), BUFFER_SIZE );

    /* Partially consume the internal buffer so that a subsequent skip has to read from the file. */
    std::vector<char> firstBytes( 1000 );
    REQUIRE_EQUAL( fileReader.read( firstBytes.data(), firstBytes.size() ), firstBytes.size() );
    REQUIRE_EQUAL( fileReader.tell(), firstBytes.size() );

    /* A null destination means skipping bytes. The remaining buffered bytes are fewer than the requested amount,
     * so this takes the refill path, which must not derive a destination pointer from the null buffer. */
    const size_t skipSize = BUFFER_SIZE - firstBytes.size() + 10;
    REQUIRE_EQUAL( fileReader.read( nullptr, skipSize ), skipSize );
    REQUIRE_EQUAL( fileReader.tell(), firstBytes.size() + skipSize );

    /* Also exercise the direct path for reads larger than the buffer. */
    const size_t largeSkip = 2 * BUFFER_SIZE;
    REQUIRE_EQUAL( fileReader.read( nullptr, largeSkip ), largeSkip );
    REQUIRE_EQUAL( fileReader.tell(), firstBytes.size() + skipSize + largeSkip );

    /* The skip must not have changed where the following read takes its data from. */
    const auto position = fileReader.tell();
    std::vector<char> readData( 100 );
    REQUIRE_EQUAL( fileReader.read( readData.data(), readData.size() ), readData.size() );
    REQUIRE( std::equal( readData.begin(), readData.end(),
                         fileContents.begin() + static_cast<std::ptrdiff_t>( position ) ) );
}


int
main()
{
    const std::string tmpFileName = "testFileReader-test-file.tmp";
    const auto tmpFileContents = fillFile( tmpFileName );
    std::cerr << "Written data: " << tmpFileContents << "\n";
    std::cerr << "Wrote " << tmpFileContents.size() << " B\n";

    testUniqueFilePointer( tmpFileName, tmpFileContents );

    const auto standardFileReader = std::make_unique<StandardFileReader>( tmpFileName );
    testFileReader( tmpFileContents, standardFileReader.get() );

    const auto bufferedFileReader =
        std::make_unique<BufferedFileReader>( std::make_unique<StandardFileReader>( tmpFileName ) );
    testFileReader( tmpFileContents, bufferedFileReader.get() );

    const auto memoryFileReader =
        std::make_unique<BufferedFileReader>( std::vector<char>( tmpFileContents.begin(), tmpFileContents.end() ) );
    testFileReader( tmpFileContents, memoryFileReader.get() );

    const auto bufferedSeekFileName = std::string( tmpFileName ) + "-buffered-seek.tmp";
    testBufferedFileReaderSeek( bufferedSeekFileName );
    std::remove( bufferedSeekFileName.c_str() );

    const auto bufferedNullFileName = std::string( tmpFileName ) + "-buffered-null.tmp";
    testBufferedFileReaderNullBuffer( bufferedNullFileName );
    std::remove( bufferedNullFileName.c_str() );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
