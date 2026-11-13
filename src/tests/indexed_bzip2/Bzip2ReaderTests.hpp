#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <core/common.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <indexed_bzip2/BZ2ReaderInterface.hpp>

#include "Bzip2TestHelpers.hpp"


namespace bzip2_test
{
using namespace rapidgzip;
using namespace indexed_bzip2;

/**
 * The serial and parallel readers are expected to behave interchangeably for the public BZ2ReaderInterface.
 * Every test taking a Policy is instantiated for both readers by the two test executables.
 *
 * A policy provides:
 *  - Reader: the reader type.
 *  - make(file, parallelization): constructs a reader for the given input.
 *  - joinThreads(reader): stops background threads; a no-op for the serial reader.
 */
template<typename Reader>
void
testSeek( size_t                            decodedFileSize,
          std::ifstream&                    decodedFile,
          const std::unique_ptr<Reader>&    encodedFile,
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

    /* Wanted differing behavior between std::ifstream and the readers. */
    REQUIRE_EQUAL( std::min<long long int>( newSeekPosDecoded, decodedFileSize ), newSeekPosEncoded );
    REQUIRE_EQUAL( std::min<long long int>( decodedFile.tellg(), decodedFileSize ),
                   static_cast<long long int>( encodedFile->tell() ) );
}


template<typename Reader>
void
testRead( std::ifstream&                    decodedFile,
          const std::unique_ptr<Reader>&    encodedFile,
          size_t                            nBytesToRead )
{
    std::cerr << "Read " << nBytesToRead << "B from " << encodedFile->tell() << "\n";

    std::vector<char> decodedBuffer( nBytesToRead, 11 );
    std::vector<char> encodedBuffer( nBytesToRead, 22 );

    if ( !encodedFile->eof() ) {
        REQUIRE_EQUAL( static_cast<size_t>( decodedFile.tellg() ), encodedFile->tell() );
    }

    /* Why doesn't the ifstream have a similar return specifying the number of read bytes?! */
    decodedFile.read( decodedBuffer.data(), static_cast<std::streamsize>( nBytesToRead ) );
    const auto nBytesReadDecoded = decodedFile.gcount();

    const auto nBytesReadEncoded = encodedFile->read( -1, encodedBuffer.data(), nBytesToRead );

    REQUIRE_EQUAL( static_cast<size_t>( nBytesReadDecoded ), nBytesReadEncoded );

    decodedBuffer.resize( static_cast<size_t>( nBytesReadDecoded ) );
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


template<typename Policy>
void
testSimpleOpenAndClose( const std::string& bz2File,
                        const size_t       parallelization )
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    {
        auto encodedFile = Policy::make( std::make_unique<StandardFileReader>( bz2File ), parallelization );
        REQUIRE_EQUAL( encodedFile->tellCompressed(), 0U );
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::duration<double> >( t1 - t0 ).count();
        REQUIRE( dt < 1 );
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double> >( t1 - t0 ).count();
    REQUIRE( dt < 1 );
}


/**
 * Tests are in such a way that seeking and reading are mirrored on the reader and the decoded file.
 * Then all read results can be checked against each other. Same for the result of tell.
 */
template<typename Policy>
void
testDecodingBz2ForFirstTime( const std::string& decodedTestFilePath,
                             const std::string& encodedTestFilePath,
                             const size_t       parallelization )
{
    const auto decodedFileSize = fileSize( decodedTestFilePath );
    std::cerr << "Decoded file size: " << decodedFileSize << "\n";

    std::ifstream decodedFile( decodedTestFilePath, std::ios_base::in | std::ios_base::binary );
    auto encodedFile = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );

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

    /* Test stopping background threads mid-stream, which is intended for FUSE. */
    seek( 333 );
    Policy::joinThreads( *encodedFile );
    read( 100 );
    seek( 222 );
    read( 1_Mi );
    read( decodedFileSize + 1000 );

    Policy::joinThreads( *encodedFile );
    seek( 1 );
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
    encodedFile = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    for ( int i = 0; i < 5; ++i ) {
        read( 1 );
    }

    std::cerr << "Test block offset loading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );

    std::cerr << "Test block offset loading after partial reading\n";
    decodedFile.clear();
    decodedFile.seekg( 0 );
    encodedFile = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );
    read( 4 );
    encodedFile->setBlockOffsets( blockOffsets );

    std::cerr << "Try reading 1B before the end of file\n";
    seek( static_cast<long long int>( decodedFileSize ) - 4 );
    read( decodedFileSize + 1000 );
}


template<typename Policy>
void
testIndexImportExport( const std::string& decodedTestFilePath,
                       const std::string& encodedTestFilePath,
                       const size_t       parallelization )
{
    auto original = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );
    const auto expectedOffsets = original->blockOffsets();

    std::vector<char> indexData;
    original->exportIndex( [&indexData] ( const void* buffer, uint64_t size )
                           {
                               const auto* const bytes = reinterpret_cast<const char*>( buffer );
                               indexData.insert( indexData.end(), bytes, bytes + size );
                           } );
    REQUIRE( !indexData.empty() );
    REQUIRE( std::find( indexData.begin(), indexData.end(), ',' ) != indexData.end() );

    const auto indexCopy = indexData;
    REQUIRE( readBzip2Index( std::make_unique<BufferViewFileReader>( indexCopy ) ) == expectedOffsets );

    auto imported = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );
    imported->importIndex( std::make_unique<BufferViewFileReader>( indexData ) );
    REQUIRE( imported->availableBlockOffsets() == expectedOffsets );
    REQUIRE( imported->blockOffsets() == expectedOffsets );

    REQUIRE( bzip2_test::readAll( *imported ) == readFile<std::vector<char> >( decodedTestFilePath ) );
}


template<typename Policy>
void
testInvalidBlockOffsetsLeaveReaderUsable( const std::string& bz2File,
                                          const size_t       parallelization )
{
    auto reader = Policy::make( std::make_unique<StandardFileReader>( bz2File ), parallelization );

    bool threw{ false };
    try {
        reader->setBlockOffsets( { { 0, 0 } } );
    } catch ( const std::invalid_argument& ) {
        threw = true;
    }
    REQUIRE( threw );

    REQUIRE_EQUAL( bzip2_test::readAll( *reader ).size(), 2_Mi );
}


template<typename Policy>
void
testIncompleteIndexIsRejected( const std::string& encodedTestFilePath,
                               const size_t       parallelization )
{
    const auto complete =
        Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization )->blockOffsets();
    REQUIRE( complete.size() >= 4 );

    const auto expectRejected =
        [&] ( const std::map<size_t, size_t>& offsets )
        {
            auto reader = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ),
                                        parallelization );
            bool threw{ false };
            try {
                reader->setBlockOffsets( offsets );
            } catch ( const std::invalid_argument& ) {
                threw = true;
            }
            REQUIRE( threw );
        };

    /* Missing end sentinel: the last entry is the EOS block and the penultimate one is a data block. */
    {
        auto offsets = complete;
        offsets.erase( std::prev( offsets.end() ) );
        expectRejected( offsets );
    }

    /* Truncated after a data block, as produced when exporting an index for only a decoded range. */
    {
        auto offsets = complete;
        offsets.erase( std::prev( offsets.end() ) );
        offsets.erase( std::prev( offsets.end() ) );
        expectRejected( offsets );
    }

    /* Missing the first checkpoint makes decoded offsets before it inaccessible. */
    {
        auto offsets = complete;
        offsets.erase( offsets.begin() );
        expectRejected( offsets );
    }
}


template<typename Policy>
void
testRunLengthOverflow( const size_t parallelization )
{
    /* A syntactically valid block whose RUNA/RUNB run length wraps uint32_t. Two literals precede
     * 31 RUNB symbols so that the run length is 2^32 - 2 and dbufCount + runLength wraps to zero,
     * which used to bypass the block-size check and write gigabytes past the block buffer. */
    const std::vector<uint8_t> compressed = {
        0x42, 0x5a, 0x68, 0x31, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x60, 0x00, 0x20, 0x00, 0x21,
        0x00, 0x82, 0x95, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xb0
    };

    auto reader = Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization );
    const auto message = bzip2_test::readToEndAndCaptureMessage( *reader );
    REQUIRE( message.find( "run length" ) != std::string::npos );
}


template<typename Policy>
void
testOversizedCodeLength( const size_t parallelization )
{
    /* A Huffman code length of 21 exceeds MAX_HUFCODE_BITS. The stream is a valid prefix up to the
     * first tree, where the 5-bit initial length is 21. The off-by-one check used to accept it and
     * index past limit[] and base[]. */
    const std::vector<uint8_t> compressed = {
        0x42, 0x5a, 0x68, 0x31, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x20, 0x00, 0x2a,
        0x80
    };
    auto reader = Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization );
    const auto message = bzip2_test::readToEndAndCaptureMessage( *reader );
    REQUIRE( message.find( "start_huffman_length 21" ) != std::string::npos );
}


template<typename Policy>
void
testEmptyAndTruncatedInputs( const std::string& /* encodedTestFilePath */,
                             const size_t       parallelization )
{
    const auto readAllOrCaptureMessage =
        [parallelization] ( const std::vector<uint8_t>& data )
        {
            auto reader = Policy::make( std::make_unique<BufferViewFileReader>( data ), parallelization );
            return bzip2_test::readToEndAndCaptureMessage( *reader );
        };

    /* An empty input has no stream to decode and must report EOF, otherwise callers that loop on read() == 0
     * while !eof() spin forever. */
    {
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( std::vector<uint8_t>{} ),
                                    parallelization );
        std::vector<char> buffer( 16 );
        REQUIRE_EQUAL( reader->read( buffer.data(), buffer.size() ), 0U );
        REQUIRE( reader->eof() );
    }

    /* A valid empty archive is a stream header followed by an EOS block. */
    const std::vector<uint8_t> validEmpty = {
        0x42, 0x5a, 0x68, 0x39, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0x00, 0x00, 0x00, 0x00
    };
    {
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( validEmpty ), parallelization );
        std::vector<char> buffer( 16 );
        REQUIRE_EQUAL( reader->read( buffer.data(), buffer.size() ), 0U );
        REQUIRE( reader->eof() );
    }

    /* A header without any block is truncated and must not be mistaken for an empty archive. */
    for ( const auto& truncated : std::vector<std::vector<uint8_t> >{
            { 'B', 'Z', 'h', '9' },
            { 'B', 'Z', 'h', '9', 0x31 },
            { 'B', 'Z', 'h', '9', 0x31, 0x41 } } ) {
        REQUIRE( !readAllOrCaptureMessage( truncated ).empty() );
    }
}


template<typename Policy>
void
testSingleBlockCRCs( const size_t parallelization )
{
    bzip2_test::checkSingleBlockCRCs(
        [parallelization] ( const std::vector<uint8_t>& data )
        {
            return Policy::make( std::make_unique<BufferViewFileReader>( data ), parallelization );
        } );
}


template<typename Policy>
void
testMultiBlockCRCVerification( const std::string& decodedTestFilePath,
                               const std::string& encodedTestFilePath,
                               const size_t       parallelization )
{
    bzip2_test::checkMultiBlockCRCs(
        readFile<std::vector<uint8_t> >( encodedTestFilePath ),
        readFile<std::vector<char> >( decodedTestFilePath ),
        [parallelization] ( const std::vector<uint8_t>& data )
        {
            return Policy::make( std::make_unique<BufferViewFileReader>( data ), parallelization );
        } );
}


template<typename Policy>
void
testImportedIndexVerifiesStreamCRC( const size_t parallelization )
{
    const auto& compressed = bzip2_test::SINGLE_BYTE_COMPRESSED_STREAM;
    const auto offsets =
        Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization )->blockOffsets();
    REQUIRE( offsets.size() >= 3 );

    auto corrupted = compressed;
    bzip2_test::corruptStreamCRC( corrupted );

    /* With an imported index, the reader still decodes every block from the beginning and therefore must
     * verify the stream CRC instead of disabling verification because the block map is finalized. */
    auto reader = Policy::make( std::make_unique<BufferViewFileReader>( corrupted ), parallelization );
    reader->setBlockOffsets( offsets );
    bzip2_test::requireStreamCRCError( bzip2_test::readToEndAndCaptureMessage( *reader ) );
}


template<typename Policy>
void
testImportedOffsetsVerifyStreamCRC( const std::string& decodedTestFilePath,
                                    const std::string& encodedTestFilePath,
                                    const size_t       parallelization )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );
    const auto offsets =
        Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization )->blockOffsets();
    REQUIRE( offsets.size() >= 4 );

    /* A valid multi-block stream must still decode completely with imported offsets. */
    {
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization );
        reader->setBlockOffsets( offsets );
        REQUIRE( bzip2_test::readAll( *reader ) == expected );
    }

    /* Imported offsets must not disable the stream CRC check for a stream read from the beginning. */
    {
        auto corrupted = compressed;
        bzip2_test::corruptStreamCRC( corrupted );
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( corrupted ), parallelization );
        reader->setBlockOffsets( offsets );
        bzip2_test::requireStreamCRCError( bzip2_test::readToEndAndCaptureMessage( *reader ) );
    }

    /* Skipping blocks makes the stream CRC unverifiable, but a valid stream must not be rejected. */
    {
        const auto seekOffset = std::next( offsets.begin() )->second + 1;
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization );
        reader->setBlockOffsets( offsets );
        REQUIRE_EQUAL( reader->seek( static_cast<long long int>( seekOffset ) ), seekOffset );
        REQUIRE( bzip2_test::readToEndAndCaptureMessage( *reader ).empty() );
    }
}


template<typename Policy>
void
testImportedDecodedSizesAreValidated( const std::string& encodedTestFilePath,
                                      const size_t       parallelization )
{
    const auto complete =
        Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization )->blockOffsets();
    REQUIRE( complete.size() >= 4 );

    /* Shift every decoded offset after the first block by one byte. The compressed offsets still form a valid
     * map, so only comparing the claimed size against the decoded block can reject it. */
    {
        auto offsets = complete;
        for ( auto it = std::next( offsets.begin() ); it != offsets.end(); ++it ) {
            REQUIRE( it->second > 0 );
            it->second -= 1;
        }

        auto reader = Policy::make( std::make_unique<StandardFileReader>( encodedTestFilePath ), parallelization );
        reader->setBlockOffsets( offsets );
        std::string message;
        try {
            reader->seek( 1 );
            message = bzip2_test::readToEndAndCaptureMessage( *reader );
        } catch ( const std::exception& exception ) {
            message = exception.what();
        }
        REQUIRE( message.find( "decoded block size" ) != std::string::npos );
    }
}


template<typename Policy>
void
testStreamCRCVerificationAfterSeekToStart( const std::string& decodedTestFilePath,
                                           const std::string& encodedTestFilePath,
                                           const size_t       parallelization )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );
    const auto offsets =
        Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization )->blockOffsets();
    REQUIRE( offsets.size() >= 4 );

    auto corrupted = compressed;
    bzip2_test::corruptStreamCRC( corrupted );

    /* Reading a prefix and then seeking back to the start must reset the accumulator. */
    {
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( corrupted ), parallelization );
        reader->setBlockOffsets( offsets );
        const auto firstBlockSize = std::next( offsets.begin() )->second;
        REQUIRE( firstBlockSize > 0 );
        std::vector<char> prefix( firstBlockSize + 128 );
        REQUIRE_EQUAL( reader->read( prefix.data(), prefix.size() ), prefix.size() );
        REQUIRE_EQUAL( reader->seek( 0 ), 0U );
        bzip2_test::requireStreamCRCError( bzip2_test::readToEndAndCaptureMessage( *reader ) );
    }

    /* Seeking forward, reading a block, and seeking back to the start must re-enable verification. */
    {
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( corrupted ), parallelization );
        reader->setBlockOffsets( offsets );
        const auto secondBlockDecodedOffset = std::next( offsets.begin() )->second;
        REQUIRE_EQUAL( reader->seek( static_cast<long long int>( secondBlockDecodedOffset + 1 ) ),
                       secondBlockDecodedOffset + 1 );
        std::vector<char> buffer( 64 );
        REQUIRE( reader->read( buffer.data(), buffer.size() ) > 0 );
        REQUIRE_EQUAL( reader->seek( 0 ), 0U );
        bzip2_test::requireStreamCRCError( bzip2_test::readToEndAndCaptureMessage( *reader ) );
    }

    /* Seeking back to the start must not make a valid stream fail the stream CRC check. */
    {
        auto reader = Policy::make( std::make_unique<BufferViewFileReader>( compressed ), parallelization );
        reader->setBlockOffsets( offsets );
        const auto firstBlockSize = std::next( offsets.begin() )->second;
        REQUIRE( firstBlockSize > 0 );
        std::vector<char> prefix( firstBlockSize + 128 );
        REQUIRE_EQUAL( reader->read( prefix.data(), prefix.size() ), prefix.size() );
        REQUIRE_EQUAL( reader->seek( 0 ), 0U );
        REQUIRE( bzip2_test::readAll( *reader ) == expected );
    }
}


template<typename Policy>
void
testConcatenatedStreamCRCVerification( const std::string& decodedTestFilePath,
                                       const std::string& encodedTestFilePath,
                                       const size_t       parallelization )
{
    const auto encoded = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    auto concatenated = encoded;
    concatenated.insert( concatenated.end(), encoded.begin(), encoded.end() );

    /* corruptStreamCRC flips the last EOS, which belongs to the second stream. */
    bzip2_test::corruptStreamCRC( concatenated );

    auto reader = Policy::make( std::make_unique<BufferViewFileReader>( concatenated ), parallelization );
    std::vector<char> firstStream( expected.size() );
    size_t nBytesRead = 0;
    while ( nBytesRead < firstStream.size() ) {
        const auto nBytesReadNow = reader->read( firstStream.data() + nBytesRead,
                                                 firstStream.size() - nBytesRead );
        if ( nBytesReadNow == 0 ) {
            break;
        }
        nBytesRead += nBytesReadNow;
    }
    REQUIRE_EQUAL( nBytesRead, firstStream.size() );
    REQUIRE( firstStream == expected );

    bzip2_test::requireStreamCRCError( bzip2_test::readToEndAndCaptureMessage( *reader ) );
}


template<typename Policy>
void
testConcatenatedStreams( const std::string& decodedTestFilePath,
                         const std::string& encodedTestFilePath,
                         const size_t       parallelization )
{
    const auto encoded = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    auto concatenated = encoded;
    concatenated.insert( concatenated.end(), encoded.begin(), encoded.end() );

    auto reader = Policy::make( std::make_unique<BufferViewFileReader>( concatenated ), parallelization );
    const auto decoded = bzip2_test::readAll( *reader );

    REQUIRE( reader->eof() );
    REQUIRE( reader->blockOffsetsComplete() );
    REQUIRE( reader->size().has_value() );
    REQUIRE_EQUAL( reader->size().value(), 2 * expected.size() );
    REQUIRE_EQUAL( decoded.size(), 2 * expected.size() );
    REQUIRE( std::equal( expected.begin(), expected.end(), decoded.begin() ) );
    REQUIRE( std::equal( expected.begin(), expected.end(),
                         decoded.begin() + static_cast<std::ptrdiff_t>( expected.size() ) ) );

    const auto singleOffsets =
        Policy::make( std::make_unique<BufferViewFileReader>( encoded ), parallelization )->blockOffsets();
    REQUIRE( reader->blockOffsets().size() > singleOffsets.size() );
}


template<typename Policy>
void
testRejectsIndexForFirstConcatenatedStream( const std::string& encodedTestFilePath,
                                            const size_t       parallelization )
{
    const auto singleStream = readFile<std::vector<char> >( encodedTestFilePath );
    auto concatenated = singleStream;
    concatenated.insert( concatenated.end(), singleStream.begin(), singleStream.end() );

    const auto firstStreamOffsets =
        Policy::make( std::make_unique<BufferViewFileReader>( singleStream ), parallelization )->blockOffsets();
    REQUIRE( firstStreamOffsets.size() >= 2 );

    auto reader = Policy::make( std::make_unique<BufferViewFileReader>( concatenated ), parallelization );
    std::string message;
    bool threw{ false };
    try {
        reader->setBlockOffsets( firstStreamOffsets );
    } catch ( const std::exception& exception ) {
        threw = true;
        message = exception.what();
    }
    REQUIRE( threw );
    REQUIRE( message.find( "another bzip2 stream" ) != std::string::npos );
}
}  // namespace bzip2_test
