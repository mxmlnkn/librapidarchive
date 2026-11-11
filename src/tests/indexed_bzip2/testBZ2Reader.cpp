#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <core/common.hpp>
#include <core/DataGenerators.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <filereader/Standard.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/IndexFileFormat.hpp>


using namespace rapidgzip;
using namespace indexed_bzip2;


void
testSimpleOpenAndClose( const std::string& bz2File )
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    {
        BZ2Reader encodedFile( std::make_unique<StandardFileReader>( bz2File ) );
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::duration<double> >( t1 - t0 ).count();
        REQUIRE( dt < 1 );
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double> >( t1 - t0 ).count();
    REQUIRE( dt < 1 );
}


void
testSeek( size_t                            decodedFileSize,
          std::ifstream&                    decodedFile,
          const std::unique_ptr<BZ2Reader>& encodedFile,
          long long int                     offset,
          int                               origin = SEEK_SET )
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
}


void
testRead( std::ifstream&                    decodedFile,
          const std::unique_ptr<BZ2Reader>& encodedFile,
          size_t                            nBytesToRead )
{
    std::cerr << "Read " << nBytesToRead << "B from " << encodedFile->tell() << "\n";

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
}


/**
 * Tests are in such a way that seeking and reading are mirrored on the BZ2Reader file and the decoded file.
 * Then all read results can be checked against each other. Same for the result of tell.
 */
void
testDecodingBz2ForFirstTime( const std::string& decodedTestFilePath,
                             const std::string& encodedTestFilePath )
{
    size_t decodedFileSize = fileSize( decodedTestFilePath );
    std::cerr << "Decoded file size: " << decodedFileSize << "\n";

    std::ifstream decodedFile( decodedTestFilePath, std::ios_base::in | std::ios_base::binary );
    auto encodedFile = std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );

    const auto seek =
        [&] ( long long int offset,
              int           origin = SEEK_SET )
        { testSeek( decodedFileSize, decodedFile, encodedFile, offset, origin ); };

    const auto read = [&] ( size_t nBytesToRead ) { testRead( decodedFile, encodedFile, nBytesToRead ); };

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
    read( 128_Mi );

    /* Try out seeking. */
    seek( 0 );
    seek( 1 );
    seek( 2 );
    seek( 2 );
    seek( 4 );
    seek( 256 );
    seek( 3_Mi );

    /* Seek after end of file */
    seek( static_cast<long long int>( decodedFileSize ) + 1000 );

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

    std::cerr << "Test block offset loading\n";
    const auto blockOffsets = encodedFile->blockOffsets();
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );

    std::cerr << "Test block offset loading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    for ( int i = 0; i < 5; ++i ) {
        read( 1 );
    }

    std::cerr << "Test block offset loading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );

    std::cerr << "Test block offset loading after partial reading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    read( 4 );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );
}


void
testIndexImportExport( const std::string& decodedTestFilePath,
                       const std::string& encodedTestFilePath )
{
    BZ2Reader original( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    const auto expectedOffsets = original.blockOffsets();

    std::vector<char> indexData;
    original.exportIndex( [&indexData] ( const void* buffer, uint64_t size )
                          {
                              const auto* const bytes = reinterpret_cast<const char*>( buffer );
                              indexData.insert( indexData.end(), bytes, bytes + size );
                          } );
    REQUIRE( !indexData.empty() );
    REQUIRE( std::find( indexData.begin(), indexData.end(), ',' ) != indexData.end() );

    const auto indexCopy = indexData;
    REQUIRE( readBzip2Index( std::make_unique<BufferViewFileReader>( indexCopy ) ) == expectedOffsets );

    BZ2Reader imported( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    imported.importIndex( std::make_unique<BufferViewFileReader>( indexData ) );
    REQUIRE( imported.availableBlockOffsets() == expectedOffsets );
    REQUIRE( imported.blockOffsets() == expectedOffsets );

    std::vector<char> decoded;
    std::vector<char> buffer( 64_Ki );
    while ( const auto nBytesRead = imported.read( -1, buffer.data(), buffer.size() ) ) {
        decoded.insert( decoded.end(), buffer.begin(), buffer.begin() + nBytesRead );
    }
    REQUIRE( decoded == readFile<std::vector<char> >( decodedTestFilePath ) );
}


void
testSeekBeforeOffsetCompletion( const std::string& decodedTestFilePath,
                                const std::string& encodedTestFilePath )
{
    size_t decodedFileSize = fileSize( decodedTestFilePath );
    std::cerr << "Decoded file size: " << decodedFileSize << "\n";

    const auto blockOffsets =
        std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) )->blockOffsets();

    std::ifstream decodedFile( decodedTestFilePath, std::ios_base::in | std::ios_base::binary );
    auto encodedFile = std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );

    const auto seek =
        [&] ( long long int offset,
              int           origin = SEEK_SET )
        { testSeek( decodedFileSize, decodedFile, encodedFile, offset, origin ); };

    const auto read = [&] ( size_t nBytesToRead ) { testRead( decodedFile, encodedFile, nBytesToRead ); };

    /* Read a bit because having a non-zero decoded count is a prerequisite to trigger a possible bug. */
    REQUIRE( encodedFile->availableBlockOffsets().empty() );
    read( 50'000 );  /* Some value smaller than the first block. */

    std::cerr << "Current block offsets after seeking 50 KB:\n";
    for ( const auto& [encodedOffset, decodedOffset] : encodedFile->availableBlockOffsets() ) {
        std::cerr << "  " << encodedOffset << " b -> " << decodedOffset << " B\n";
    }
    /* Like ParallelBZ2Reader, a block is added only after it has been fully decoded. */
    REQUIRE( encodedFile->availableBlockOffsets().empty() );
    /* Seek back, which triggers redecoding parts leading to the internal variable being incremented. */
    seek( 0 );
    read( 20'000 );
    REQUIRE( encodedFile->tell() == 20'000 );

    REQUIRE( blockOffsets == encodedFile->blockOffsets() );
    REQUIRE( blockOffsets.size() > 1 &&
             "Cannot trigger the possible bug with only one real block! use a larger file." );
}


void
testEmptyInput()
{
    /* An empty input has no stream to decode and must report EOF, otherwise callers that loop on read() == 0
     * while !eof() spin forever, which the CLI with --buffer-size does. */
    const std::vector<uint8_t> compressed;
    BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
    std::vector<char> buffer( 16 );
    REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), 0U );
    REQUIRE( reader.eof() );
    REQUIRE( reader.size().has_value() );
    REQUIRE_EQUAL( reader.size().value(), 0U );
    REQUIRE_EQUAL( reader.tell(), 0U );
}


void
testHeaderOnlyInput()
{
    /* A header without any block is truncated and must not be mistaken for an empty archive. */
    const std::vector<uint8_t> compressed = { 'B', 'Z', 'h', '9' };
    BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
    std::vector<char> buffer( 16 );
    bool threw{ false };
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& ) {
        threw = true;
    }
    REQUIRE( threw );
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

    BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
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
        BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
        std::vector<char> buffer( 1 );
        REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), buffer.size() );
        REQUIRE_EQUAL( buffer.front(), 'A' );
    }

    /* A mismatching block CRC must be rejected after the block has been fully decoded. */
    auto corrupted = compressed;
    corrupted[13] ^= uint8_t( 1U << 0U );
    BZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ) );
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
    BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
    std::vector<char> buffer( 16 );
    std::string message;
    try {
        while ( reader.read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "start_huffman_length 21" ) != std::string::npos );
}


int
main()
{
    testEmptyInput();
    testHeaderOnlyInput();
    testRunLengthOverflow();
    testBlockCRCVerification();
    testOversizedCodeLength();

    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testBZ2Reader" );

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

        testDecodingBz2ForFirstTime( decodedTestFilePath.string(), encodedTestFilePath.string() );

        /* This test works because any seeking back triggers the completion of the block offset map! */
        testSeekBeforeOffsetCompletion( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testIndexImportExport( decodedTestFilePath.string(), encodedTestFilePath.string() );
    } catch ( const std::exception& exception ) {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
