#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h>
#endif

#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/BufferView.hpp>
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


struct FileReaderOperation
{
    enum class Type
    {
        READ,
        SEEK,
    };

    Type type{ Type::READ };
    size_t readSize{ 0 };
    bool skip{ false };
    long long int seekOffset{ 0 };
    int seekOrigin{ SEEK_SET };
};


void
applyOperation( FileReader&                 first,
                FileReader&                 second,
                const FileReaderOperation&  operation )
{
    switch ( operation.type )
    {
    case FileReaderOperation::Type::READ:
    {
        std::vector<char> firstData( operation.readSize );
        std::vector<char> secondData( operation.readSize );
        const auto firstBytesRead = first.read( operation.skip ? nullptr : firstData.data(), operation.readSize );
        const auto secondBytesRead = second.read( operation.skip ? nullptr : secondData.data(), operation.readSize );
        REQUIRE_EQUAL( firstBytesRead, secondBytesRead );
        if ( !operation.skip ) {
            REQUIRE( firstData == secondData );
        }
        break;
    }
    case FileReaderOperation::Type::SEEK:
    {
        const auto firstPosition = first.seek( operation.seekOffset, operation.seekOrigin );
        const auto secondPosition = second.seek( operation.seekOffset, operation.seekOrigin );
        REQUIRE_EQUAL( firstPosition, secondPosition );
        break;
    }
    }

    REQUIRE_EQUAL( first.tell(), second.tell() );
    REQUIRE_EQUAL( first.eof(), second.eof() );
}


void
testBufferedFileReaderRetainsCache( const std::string& tmpFileName )
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

    BufferedFileReader bufferedReader( std::make_unique<StandardFileReader>( tmpFileName ), BUFFER_SIZE );
    StandardFileReader reference( tmpFileName );

    /* Advance to 150 KiB so that the active cache window starts at 128 KiB. */
    for ( size_t i = 0; i < 150; ++i ) {
        applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 1_Ki, false, 0, SEEK_SET } );
    }
    REQUIRE_EQUAL( bufferedReader.tell(), 150_Ki );

    /* The first 42 KiB of this read come from the cache and the remainder bypasses it. */
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 200_Ki, false, 0, SEEK_SET } );
    REQUIRE_EQUAL( bufferedReader.tell(), 350_Ki );

    /* Seek back into the retained [128 KiB, 192 KiB) range and read from it. */
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::SEEK, 0, false, 150_Ki, SEEK_SET } );
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 1_Ki, false, 0, SEEK_SET } );

    /* Seek to the retained range's end. Reading there refills and repositions the underlying file. */
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::SEEK, 0, false, 192_Ki, SEEK_SET } );
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 1_Ki, false, 0, SEEK_SET } );

    /* Seek before, and then after, the newly active cache window. */
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::SEEK, 0, false, 100_Ki, SEEK_SET } );
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 1_Ki, false, 0, SEEK_SET } );
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::SEEK, 0, false, 300_Ki, SEEK_SET } );
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 1_Ki, false, 0, SEEK_SET } );

    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::SEEK, 0, false, -10, SEEK_END } );
    applyOperation( bufferedReader, reference, { FileReaderOperation::Type::READ, 10, false, 0, SEEK_SET } );
}


void
testBufferedFileReaderDifferential( const std::string& tmpFileName )
{
    constexpr size_t FILE_SIZE = 256_Ki;
    constexpr size_t BUFFER_SIZE = 4_Ki;

    std::vector<char> fileContents( FILE_SIZE );
    for ( size_t i = 0; i < fileContents.size(); ++i ) {
        fileContents[i] = static_cast<char>( ( i * 17 + i / 251 ) % 251 );
    }
    {
        std::ofstream outFile( tmpFileName, std::ios_base::out | std::ios_base::binary );
        outFile.write( fileContents.data(), static_cast<std::streamsize>( fileContents.size() ) );
    }

    const std::vector<size_t> readSizes = {
        0, 1, 17, 255, BUFFER_SIZE - 1, BUFFER_SIZE, BUFFER_SIZE + 1, 2 * BUFFER_SIZE, 3 * BUFFER_SIZE + 17
    };

    std::vector<FileReaderOperation> operations;
    operations.emplace_back( FileReaderOperation{ FileReaderOperation::Type::SEEK, 0, false, 0, SEEK_SET } );

    /* Explicitly cover every read size with and without a destination buffer. */
    for ( const auto readSize : readSizes ) {
        operations.emplace_back( FileReaderOperation{ FileReaderOperation::Type::READ, readSize, false, 0, SEEK_SET } );
        operations.emplace_back( FileReaderOperation{ FileReaderOperation::Type::READ, readSize, true, 0, SEEK_SET } );
    }
    operations.emplace_back( FileReaderOperation{ FileReaderOperation::Type::SEEK, 0, false, 0, SEEK_SET } );

    std::mt19937_64 randomEngine{ 12345 };
    std::uniform_int_distribution<int> operationDistribution( 0, 99 );
    std::uniform_int_distribution<size_t> readSizeDistribution( 0, readSizes.size() - 1 );
    std::uniform_int_distribution<int> skipDistribution( 0, 3 );
    std::uniform_int_distribution<long long int> targetPositionDistribution(
        0, static_cast<long long int>( FILE_SIZE ) );
    std::uniform_int_distribution<int> seekOriginDistribution( 0, 2 );
    const int seekOrigins[] = { SEEK_SET, SEEK_CUR, SEEK_END };

    /* Generate only seeks that both readers accept. StandardFileReader rejects relative seeks before the start
     * and does not clamp absolute seeks, while BufferedFileReader clamps with effectiveOffset. */
    long long int position = 0;
    constexpr size_t N_OPERATIONS = 2000;
    for ( size_t i = 0; i < N_OPERATIONS; ++i ) {
        if ( operationDistribution( randomEngine ) < 60 ) {
            const auto readSize = readSizes[readSizeDistribution( randomEngine )];
            const auto skip = skipDistribution( randomEngine ) == 0;
            operations.emplace_back( FileReaderOperation{ FileReaderOperation::Type::READ, readSize, skip, 0,
                                                          SEEK_SET } );
            position += static_cast<long long int>( std::min( readSize, FILE_SIZE - static_cast<size_t>( position ) ) );
        } else {
            const auto target = targetPositionDistribution( randomEngine );
            const auto origin = seekOrigins[seekOriginDistribution( randomEngine )];
            long long int offset = target;
            if ( origin == SEEK_CUR ) {
                offset = target - position;
            } else if ( origin == SEEK_END ) {
                offset = target - static_cast<long long int>( FILE_SIZE );
            }
            operations.emplace_back( FileReaderOperation{ FileReaderOperation::Type::SEEK, 0, false, offset, origin } );
            position = target;
        }
    }

    {
        BufferedFileReader bufferedReader( std::make_unique<StandardFileReader>( tmpFileName ), BUFFER_SIZE );
        StandardFileReader reference( tmpFileName );
        for ( const auto& operation : operations ) {
            applyOperation( bufferedReader, reference, operation );
        }
    }

    {
        BufferedFileReader bufferedReader( fileContents, BUFFER_SIZE );
        BufferViewFileReader reference( fileContents );
        for ( const auto& operation : operations ) {
            applyOperation( bufferedReader, reference, operation );
        }
    }
}


void
testNullBufferSkipAtEnd( FileReader&                fileReader,
                         const std::vector<char>&   fileContents )
{
    REQUIRE_EQUAL( fileReader.tell(), size_t( 0 ) );

    std::vector<char> firstBytes( 100 );
    REQUIRE_EQUAL( fileReader.read( firstBytes.data(), firstBytes.size() ), firstBytes.size() );
    REQUIRE( std::equal( firstBytes.begin(), firstBytes.end(), fileContents.begin() ) );

    /* A zero-length null read must not change the position. */
    REQUIRE_EQUAL( fileReader.read( nullptr, 0 ), size_t( 0 ) );
    REQUIRE_EQUAL( fileReader.tell(), firstBytes.size() );

    /* Skipping past the end returns only the bytes that are still available. */
    REQUIRE_EQUAL( fileReader.read( nullptr, fileContents.size() ), fileContents.size() - firstBytes.size() );
    REQUIRE_EQUAL( fileReader.tell(), fileContents.size() );
    REQUIRE( fileReader.eof() );

    REQUIRE_EQUAL( fileReader.read( nullptr, 0 ), size_t( 0 ) );
    REQUIRE_EQUAL( fileReader.read( nullptr, 100 ), size_t( 0 ) );
    REQUIRE_EQUAL( fileReader.tell(), fileContents.size() );
}


void
testStandardFileReaderNullBufferBeyondEnd( const std::string& tmpFileName,
                                           size_t             fileSize )
{
    StandardFileReader fileReader( tmpFileName );

    /* Seeking beyond the end is allowed, but a skip may only report and advance over available bytes. */
    const auto beyondEndOffset = 2 * fileSize;
    REQUIRE_EQUAL( fileReader.seek( static_cast<long long int>( beyondEndOffset ) ), beyondEndOffset );
    REQUIRE_EQUAL( fileReader.read( nullptr, 100 ), size_t( 0 ) );
    REQUIRE_EQUAL( fileReader.tell(), beyondEndOffset );

    /* A non-null read behaves the same way, so the two forms must not disagree. */
    char byte{ 0 };
    REQUIRE_EQUAL( fileReader.read( &byte, 1 ), size_t( 0 ) );
    REQUIRE_EQUAL( fileReader.tell(), beyondEndOffset );
}


#if !defined(_WIN32)
void
testStandardFileReaderNullBufferOnPipe()
{
    std::vector<char> fileContents( 100 );
    for ( size_t i = 0; i < fileContents.size(); ++i ) {
        fileContents[i] = static_cast<char>( i % 251 );
    }

    int fileDescriptors[2] = { -1, -1 };
    REQUIRE_EQUAL( ::pipe( fileDescriptors ), 0 );
    REQUIRE_EQUAL( ::write( fileDescriptors[1], fileContents.data(), fileContents.size() ),
                   static_cast<ssize_t>( fileContents.size() ) );
    REQUIRE_EQUAL( ::close( fileDescriptors[1] ), 0 );

    StandardFileReader fileReader( fileDescriptors[0] );
    REQUIRE( !fileReader.seekable() );
    /* A pipe reports st_size == 0, which must not be mistaken for a real file size. */
    REQUIRE( !fileReader.size().has_value() );

    /* A skip must not consume or report more than requested even if the pipe contains more data. */
    REQUIRE_EQUAL( fileReader.read( nullptr, 1 ), size_t( 1 ) );
    REQUIRE_EQUAL( fileReader.tell(), size_t( 1 ) );
    REQUIRE( !fileReader.eof() );

    REQUIRE_EQUAL( fileReader.read( nullptr, fileContents.size() ), fileContents.size() - 1 );
    REQUIRE_EQUAL( fileReader.tell(), fileContents.size() );
    REQUIRE( fileReader.eof() );

    REQUIRE_EQUAL( fileReader.read( nullptr, 100 ), size_t( 0 ) );

    REQUIRE_EQUAL( ::close( fileDescriptors[0] ), 0 );
}


void
testNonRegularFileHasUnknownSize()
{
    StandardFileReader reader( "/dev/null" );
    REQUIRE( !reader.seekable() );
    REQUIRE( !reader.size().has_value() );

    char byte{ 0 };
    REQUIRE_EQUAL( reader.read( &byte, 1 ), size_t( 0 ) );
    REQUIRE( reader.eof() );
}


void
testBufferedFileReaderRetainsCacheOnPipe()
{
    constexpr size_t FILE_SIZE = 20_Ki;
    constexpr size_t BUFFER_SIZE = 4_Ki;

    std::vector<char> fileContents( FILE_SIZE );
    for ( size_t i = 0; i < fileContents.size(); ++i ) {
        fileContents[i] = static_cast<char>( ( i * 13 + i / 251 ) % 251 );
    }

    int fileDescriptors[2] = { -1, -1 };
    REQUIRE_EQUAL( ::pipe( fileDescriptors ), 0 );
    REQUIRE_EQUAL( ::write( fileDescriptors[1], fileContents.data(), fileContents.size() ),
                   static_cast<ssize_t>( fileContents.size() ) );
    REQUIRE_EQUAL( ::close( fileDescriptors[1] ), 0 );

    BufferedFileReader reader( std::make_unique<StandardFileReader>( fileDescriptors[0] ), BUFFER_SIZE );
    REQUIRE( !reader.seekable() );
    REQUIRE( !reader.size().has_value() );

    std::vector<char> firstBytes( 1_Ki );
    REQUIRE_EQUAL( reader.read( firstBytes.data(), firstBytes.size() ), firstBytes.size() );
    REQUIRE( std::equal( firstBytes.begin(), firstBytes.end(), fileContents.begin() ) );

    /* This read consumes the rest of the cache and then bypasses it with a direct read. The cache is retained. */
    std::vector<char> directBytes( 8_Ki );
    REQUIRE_EQUAL( reader.read( directBytes.data(), directBytes.size() ), directBytes.size() );
    REQUIRE( std::equal( directBytes.begin(), directBytes.end(),
                         fileContents.begin() + static_cast<std::ptrdiff_t>( firstBytes.size() ) ) );

    /* Seek back into the retained [0, 4 KiB) range. A non-seekable reader can serve this without touching the file. */
    REQUIRE_EQUAL( reader.seek( 2_Ki, SEEK_SET ), 2_Ki );
    std::vector<char> cachedBytes( 2_Ki );
    REQUIRE_EQUAL( reader.read( cachedBytes.data(), cachedBytes.size() ), cachedBytes.size() );
    REQUIRE( std::equal( cachedBytes.begin(), cachedBytes.end(),
                         fileContents.begin() + static_cast<std::ptrdiff_t>( 2_Ki ) ) );

    REQUIRE_EQUAL( ::close( fileDescriptors[0] ), 0 );
}
#endif


void
testBufferViewFileReaderNullBuffer()
{
    std::vector<char> fileContents( 512_Ki );
    for ( size_t i = 0; i < fileContents.size(); ++i ) {
        fileContents[i] = static_cast<char>( i % 251 );
    }

    BufferViewFileReader fileReader( fileContents );

    std::vector<char> firstBytes( 1000 );
    REQUIRE_EQUAL( fileReader.read( firstBytes.data(), firstBytes.size() ), firstBytes.size() );
    REQUIRE( std::equal( firstBytes.begin(), firstBytes.end(), fileContents.begin() ) );

    /* A null destination means skipping bytes and must not be passed to memcpy. */
    const size_t skipSize = 5000;
    REQUIRE_EQUAL( fileReader.read( nullptr, skipSize ), skipSize );
    REQUIRE_EQUAL( fileReader.tell(), firstBytes.size() + skipSize );

    const auto position = fileReader.tell();
    std::vector<char> readData( 100 );
    REQUIRE_EQUAL( fileReader.read( readData.data(), readData.size() ), readData.size() );
    REQUIRE( std::equal( readData.begin(), readData.end(),
                         fileContents.begin() + static_cast<std::ptrdiff_t>( position ) ) );
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

    const auto bufferedCacheFileName = std::string( tmpFileName ) + "-buffered-cache.tmp";
    testBufferedFileReaderRetainsCache( bufferedCacheFileName );
    std::remove( bufferedCacheFileName.c_str() );

    const auto bufferedDifferentialFileName = std::string( tmpFileName ) + "-buffered-differential.tmp";
    testBufferedFileReaderDifferential( bufferedDifferentialFileName );
    std::remove( bufferedDifferentialFileName.c_str() );

    /* All readers must agree on the skip semantics, including zero-length and past-the-end skips. */
    const std::vector<char> contents( tmpFileContents.begin(), tmpFileContents.end() );
    {
        StandardFileReader standardReader( tmpFileName );
        testNullBufferSkipAtEnd( standardReader, contents );
    }
    {
        BufferedFileReader bufferedReader( std::make_unique<StandardFileReader>( tmpFileName ) );
        testNullBufferSkipAtEnd( bufferedReader, contents );
    }
    {
        BufferViewFileReader bufferViewReader( contents );
        testNullBufferSkipAtEnd( bufferViewReader, contents );
    }

    testStandardFileReaderNullBufferBeyondEnd( tmpFileName, tmpFileContents.size() );

#if !defined(_WIN32)
    testStandardFileReaderNullBufferOnPipe();
    testNonRegularFileHasUnknownSize();
    testBufferedFileReaderRetainsCacheOnPipe();
#endif

    testBufferViewFileReaderNullBuffer();

    const auto bufferedNullFileName = std::string( tmpFileName ) + "-buffered-null.tmp";
    testBufferedFileReaderNullBuffer( bufferedNullFileName );
    std::remove( bufferedNullFileName.c_str() );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
