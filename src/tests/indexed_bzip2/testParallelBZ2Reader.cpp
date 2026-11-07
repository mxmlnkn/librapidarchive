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
#include <indexed_bzip2/ParallelBZ2Reader.hpp>


using namespace rapidgzip;
using namespace indexed_bzip2;


void
testSimpleOpenAndClose( const std::string& bz2File )
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    {
        ParallelBZ2Reader encodedFile( std::make_unique<StandardFileReader>( bz2File ) );
        REQUIRE_EQUAL( encodedFile.tellCompressed(), 0U );
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::duration<double> >( t1 - t0 ).count();
        REQUIRE( dt < 1 );
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double> >( t1 - t0 ).count();
    REQUIRE( dt < 1 );
}


void
testInvalidBlockOffsetsLeaveReaderUsable( const std::string& bz2File )
{
    ParallelBZ2Reader reader( std::make_unique<StandardFileReader>( bz2File ) );

    bool threw{ false };
    try {
        reader.setBlockOffsets( { { 0, 0 } } );
    } catch ( const std::invalid_argument& ) {
        threw = true;
    }
    REQUIRE( threw );

    std::vector<char> buffer( 64_Ki );
    size_t totalBytesRead{ 0 };
    while ( const auto nBytesRead = reader.read( -1, buffer.data(), buffer.size() ) ) {
        totalBytesRead += nBytesRead;
    }
    REQUIRE_EQUAL( totalBytesRead, size_t( 2_Mi ) );
}


/**
 * Tests are in such a way that seeking and reading are mirrored on the ParallelBZ2Reader file and the decoded file.
 * Then all read results can be checked against each other. Same for the result of tell.
 */
void
testDecodingBz2ForFirstTime( const std::string& decodedTestFilePath,
                             const std::string& encodedTestFilePath )
{
    size_t decodedFileSize = 0;
    {
        std::ifstream file( decodedTestFilePath, std::ios_base::in | std::ios_base::binary );
        file.seekg( 0, std::ios::end );
        decodedFileSize = file.tellg();
    }
    std::cerr << "Decoded file size: " << decodedFileSize << "\n";

    std::ifstream decodedFile( decodedTestFilePath, std::ios_base::in | std::ios_base::binary );
    auto encodedFile =
        std::make_unique<ParallelBZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );

    const auto seek =
        [&] ( long long int offset,
              int           origin = SEEK_SET )
        {
            std::cerr << "Seek to " << offset << "\n";

            /**
             * Clear fail bit in order to seek back.
             * When using read to read the number of bytes the file has, then no eof bit is set.
             * As soon as you request one more byte than the file contains, both, the failbit and eofbit are set
             * but only the eofbit will be cleared by seekg since C++11.
             * @see https://en.cppreference.com/w/cpp/io/basic_istream/seekg
             * > Before doing anything else, seekg clears eofbit. (since C++11)
             */
            if ( decodedFile.fail() ) {
                decodedFile.clear();
            }
            decodedFile.seekg( offset, toSeekdir( origin ) );
            const auto newSeekPosDecoded = static_cast<long long int>( decodedFile.tellg() );
            const auto newSeekPosEncoded = static_cast<long long int>( encodedFile->seek( offset, origin ) );

            /* Wanted differing behavior between std::ifstream and BZ2Reader. */
            REQUIRE_EQUAL( std::min<long long int>( newSeekPosDecoded, decodedFileSize ), newSeekPosEncoded );
            REQUIRE_EQUAL( std::min<long long int>( decodedFile.tellg(), decodedFileSize ),
                           static_cast<long long int>( encodedFile->tell() ) );

            /* Beware! eof behavior is different. std:ifstream requires to read more than the file contents
             * for EOF to be reached while BZ2Reader only required to read more than >or equal< the file
             * size of bytes. Furthermore, seeking beyond the file does not set EOF in std::ifstream but does
             * set EOF in BZ2Reader! */
            //REQUIRE_EQUAL( decodedFile.eof(), encodedFile->eof() );
        };

    const auto read =
        [&] ( size_t nBytesToRead )
        {
            std::cerr << "Read " << nBytesToRead << "B\n";

            std::vector<char> decodedBuffer( nBytesToRead, 11 );
            std::vector<char> encodedBuffer( nBytesToRead, 22 );

            if ( !encodedFile->eof() ) {
                REQUIRE_EQUAL( static_cast<size_t>( decodedFile.tellg() ), encodedFile->tell() );
            }

            /* Why doesn't the ifstream has a similar return specifying the number of read bytes?! */
            decodedFile.read( decodedBuffer.data(), static_cast<std::streamsize>( nBytesToRead ) );
            const auto nBytesReadDecoded = decodedFile.gcount();

            const auto nBytesReadEncoded = encodedFile->read( -1, encodedBuffer.data(), nBytesToRead );

            REQUIRE_EQUAL( static_cast<size_t>( nBytesReadDecoded ), nBytesReadEncoded );

            decodedBuffer.resize( nBytesReadDecoded );
            encodedBuffer.resize( nBytesReadEncoded );

            /* Encountering eof during read also sets fail bit meaning tellg will return -1! */
            if ( !decodedFile.eof() ) {
                REQUIRE_EQUAL( static_cast<long long int>( decodedFile.tellg() ),
                               static_cast<long long int>( encodedFile->tell() ) );
            }
            REQUIRE_EQUAL( decodedFile.eof(), encodedFile->eof() );

            /* Avoid REQUIRE_EQUAL in order to avoid printing huge binary buffers out. */
            size_t equalElements = 0;
            size_t firstInequal = std::numeric_limits<long int>::max();
            for ( size_t i = 0; i < std::min( decodedBuffer.size(), encodedBuffer.size() ); ++i ) {
                if ( decodedBuffer[i] == encodedBuffer[i] ) {
                    ++equalElements;
                } else {
                    firstInequal = std::min( firstInequal, i );
                }
            }
            REQUIRE_EQUAL( equalElements, std::min( decodedBuffer.size(), encodedBuffer.size() ) );

            if ( equalElements != std::min( decodedBuffer.size(), encodedBuffer.size() ) ) {
                std::cerr << "First inequal element at " << firstInequal << "\n";
            }
        };

    /* Try some subsequent small reads. */
    read( 1 );
    read( 0 );
    read( 1 );
    read( 2 );
    read( 10 );
    read( 100 );
    read( 256 );

    /* Try some subsequent reads over bz2 block boundaries. */
    read( 5_Mi );
    read( 7_Mi );
    read( 1_Ki );

    /* Try reading over the end of the file. */
    read( decodedFileSize + 1000 );

    /* Try out seeking. */
    seek( 0 );
    seek( 1 );
    seek( 2 );
    seek( 2 );
    seek( 4 );
    seek( 256 );
    seek( 3_Mi );

    /* Seek after end of file */
    seek( 1_Gi );

    REQUIRE( encodedFile->blockOffsetsComplete() );
    REQUIRE_EQUAL( decodedFileSize, encodedFile->size().value() );

    /* Seek back and forth */
    seek( 10'000 );
    seek( 50'000 );
    seek( 10'000 );
    seek( 40'000 );

    /* Seek and read */
    seek( 0 );
    read( 1 );

    seek( 1 );
    read( 1 );

    seek( 2 );
    read( 2 );

    seek( 256 );
    read( 2 );

    seek( 256 );
    read( 1_Ki );

    seek( 2_Mi + 432 );
    read( 12345 );

    seek( 1_Mi - 432 );
    read( 432 );

    /* Try reading 1B before the end of file */
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    for ( int i = 0; i < 5; ++i ) {
        read( 1 );
    }

    /* Test thread joining feature intended for FUSE */
    seek( 333 );
    encodedFile->joinThreads();
    read( 100 );  /* Direct read after current position even after closing threads. */
    seek( 222 );
    read( 1_Mi );
    read( decodedFileSize + 1000 );

    encodedFile->joinThreads();
    seek( 1 );  /* Direct read after current position even after closing threads. */
    read( 100 );

    std::cerr << "Test block offset loading\n";
    const auto blockOffsets = encodedFile->blockOffsets();
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );

    std::cerr << "Test block offset loading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = std::make_unique<ParallelBZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    for ( int i = 0; i < 5; ++i ) {
        read( 1 );
    }

    std::cerr << "Test block offset loading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = std::make_unique<ParallelBZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );

    std::cerr << "Test block offset loading after partial reading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = std::make_unique<ParallelBZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    read( 4 );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );
}


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
testRunLengthOverflow()
{
    /* A syntactically valid block whose RUNA/RUNB run length wraps uint32_t. Two literals precede
     * 31 RUNB symbols so that the run length is 2^32 - 2 and dbufCount + runLength wraps to zero,
     * which used to bypass the block-size check and write gigabytes past the block buffer. */
    const std::vector<uint8_t> compressed = {
        0x42, 0x5a, 0x68, 0x31, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x60, 0x00, 0x20, 0x00, 0x21,
        0x00, 0x82, 0x95, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xb0
    };

    ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), /* parallelization */ 2 );
    std::vector<char> buffer( 4_Ki );
    std::string message;
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "run length" ) != std::string::npos );
}


void
testTruncatedStreamWithoutBlocks()
{
    const std::vector<std::vector<uint8_t> > truncated = {
        { 'B', 'Z', 'h', '9' },
        { 'B', 'Z', 'h', '9', 0x31 },
        { 'B', 'Z', 'h', '9', 0x31, 0x41 },
    };
    for ( const auto& compressed : truncated ) {
        ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), /* parallelization */ 2 );
        std::vector<char> buffer( 16 );
        bool threw{ false };
        try {
            while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
        } catch ( const std::exception& ) {
            threw = true;
        }
        REQUIRE( threw );
    }

    /* A valid empty archive is a stream header followed by an EOS block and must still decode to nothing. */
    const std::vector<uint8_t> validEmpty = {
        0x42, 0x5a, 0x68, 0x39, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0x00, 0x00, 0x00, 0x00
    };
    {
        ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( validEmpty ),
                                  /* parallelization */ 2 );
        std::vector<char> buffer( 16 );
        REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), 0U );
        REQUIRE( reader.eof() );
    }

    /* A completely empty input is accepted as an empty archive, matching the sequential reader. */
    const std::vector<uint8_t> empty;
    ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( empty ), /* parallelization */ 2 );
    std::vector<char> buffer( 16 );
    REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), 0U );
    REQUIRE( reader.eof() );
}


void
testBlockCRCVerification()
{
    /* bzip2 -c of the single byte 'A'. The stored big-endian block CRC follows the 48-bit block magic. */
    const std::vector<uint8_t> compressed = {
        0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x81, 0xb0,
        0x2d, 0x8b, 0x00, 0x00, 0x00, 0x04, 0x00, 0x20, 0x00, 0x20, 0x00, 0x21,
        0x18, 0x46, 0x82, 0xee, 0x48, 0xa7, 0x0a, 0x12, 0x10, 0x36, 0x05, 0xb1,
        0x60
    };
    {
        ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), /* parallelization */ 2 );
        std::vector<char> buffer( 1 );
        REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), buffer.size() );
        REQUIRE_EQUAL( buffer.front(), 'A' );
    }

    /* A mismatching block CRC must be rejected after the block has been fully decoded. */
    auto corrupted = compressed;
    corrupted[13] ^= uint8_t( 1U << 0U );
    ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ), /* parallelization */ 2 );
    std::vector<char> buffer( 16 );
    std::string message;
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "CRC" ) != std::string::npos );
}


void
testOversizedCodeLength()
{
    /* A Huffman code length of 21 exceeds MAX_HUFCODE_BITS. The stream is a valid prefix up to the
     * first tree, where the 5-bit initial length is 21. The off-by-one check used to accept it and
     * index past limit[] and base[]. */
    const std::vector<uint8_t> compressed = {
        0x42, 0x5a, 0x68, 0x31, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x20, 0x00, 0x2a,
        0x80
    };
    ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), /* parallelization */ 2 );
    std::vector<char> buffer( 16 );
    std::string message;
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "start_huffman_length 21" ) != std::string::npos );
}


void
corruptStreamCRC( std::vector<uint8_t>& compressed )
{
    /* Find the EOS magic at its last occurrence and flip the first bit of the following stream CRC. */
    size_t eosBitOffset{ std::numeric_limits<size_t>::max() };
    uint64_t window{ 0 };
    for ( size_t bit = 0; bit < compressed.size() * 8; ++bit ) {
        window = ( ( window << 1U ) | ( ( compressed[bit / 8] >> ( 7 - bit % 8 ) ) & 1U ) )
                 & ( ( uint64_t( 1 ) << bzip2::MAGIC_BITS_SIZE ) - 1U );
        if ( ( bit + 1 >= bzip2::MAGIC_BITS_SIZE ) && ( window == bzip2::MAGIC_BITS_EOS ) ) {
            eosBitOffset = bit + 1 - bzip2::MAGIC_BITS_SIZE;
        }
    }
    REQUIRE( eosBitOffset != std::numeric_limits<size_t>::max() );

    const auto crcBitOffset = eosBitOffset + bzip2::MAGIC_BITS_SIZE;
    compressed[crcBitOffset / 8] ^= static_cast<uint8_t>( 1U << ( 7 - crcBitOffset % 8 ) );
}


void
testStreamCRCVerification( const std::string& encodedTestFilePath )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );

    auto corrupted = compressed;
    corruptStreamCRC( corrupted );

    ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ), /* parallelization */ 2 );
    std::vector<char> buffer( 4_Ki );
    std::string message;
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "Stream CRC" ) != std::string::npos );
}


void
testStreamCRCVerificationWithImportedOffsets( const std::string& encodedTestFilePath )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );

    std::map<size_t, size_t> offsets;
    {
        ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), /* parallelization */ 2 );
        offsets = reader.blockOffsets();
    }

    auto corrupted = compressed;
    corruptStreamCRC( corrupted );

    /* Reading the whole stream sequentially must still verify the stream CRC with imported offsets. */
    {
        ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ), /* parallelization */ 2 );
        reader.setBlockOffsets( offsets );
        std::vector<char> buffer( 4_Ki );
        std::string message;
        try {
            while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
        } catch ( const std::exception& exception ) {
            message = exception.what();
        }
        REQUIRE( message.find( "Stream CRC" ) != std::string::npos );
    }

    /* Skipping blocks makes the stream CRC unverifiable, but a valid stream must not be rejected. */
    {
        REQUIRE( offsets.size() >= 3 );
        const auto secondBlock = std::next( offsets.begin() );
        const auto seekOffset = secondBlock->second + 1;

        ParallelBZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ), /* parallelization */ 2 );
        reader.setBlockOffsets( offsets );
        REQUIRE_EQUAL( reader.seek( static_cast<long long int>( seekOffset ) ), seekOffset );

        std::vector<char> buffer( 4_Ki );
        bool threw = false;
        try {
            while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
        } catch ( const std::exception& ) {
            threw = true;
        }
        REQUIRE( !threw );
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
    testRunLengthOverflow();
    testBlockCRCVerification();
    testOversizedCodeLength();
    testTruncatedStreamWithoutBlocks();

    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testParallelBZ2Reader" );

    const auto decodedTestFilePath = tmpFolder.path() / "decoded";
    createRandomTextFile( decodedTestFilePath.string(), 2_Mi );

    const auto command = "bzip2 -k -- '" + decodedTestFilePath.string() + "'";
    const auto returnCode = std::system( command.c_str() );
    if ( returnCode != 0 ) {
        std::cerr << "Failed to compress sample file\n";
        return 1;
    }
    const auto encodedTestFilePath = tmpFolder.path() / "encoded-sample.bz2";
    std::filesystem::rename( tmpFolder.path() / "decoded.bz2", encodedTestFilePath );

    try
    {
        testSimpleOpenAndClose( encodedTestFilePath.string() );
        testInvalidBlockOffsetsLeaveReaderUsable( encodedTestFilePath.string() );
        testStreamCRCVerification( encodedTestFilePath.string() );
        testStreamCRCVerificationWithImportedOffsets( encodedTestFilePath.string() );
        testJoinThreadsMidStream( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testDecodingBz2ForFirstTime( decodedTestFilePath.string(), encodedTestFilePath.string() );
    } catch ( const std::exception& exception ) {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
