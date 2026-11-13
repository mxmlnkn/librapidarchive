#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <core/common.hpp>
#include <core/DataGenerators.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <filereader/Standard.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/IndexFileFormat.hpp>

#include "Bzip2TestHelpers.hpp"


using namespace rapidgzip;
using namespace indexed_bzip2;


class NonSeekableFileReader final :
    public FileReader
{
public:
    explicit
    NonSeekableFileReader( std::vector<char> data ) :
        m_data( std::move( data ) )
    {}

    void close() override { m_closed = true; }
    [[nodiscard]] bool closed() const override { return m_closed; }
    /** EOF is only reported after a read past the end, like a socket or pipe that has not been probed yet. */
    [[nodiscard]] bool eof() const override { return m_eof; }
    [[nodiscard]] bool fail() const override { return false; }
    [[nodiscard]] int fileno() const override { return -1; }
    [[nodiscard]] bool seekable() const override { return false; }

    [[nodiscard]] size_t
    read( char* buffer, size_t nMaxBytesToRead ) override
    {
        if ( m_position >= m_data.size() ) {
            m_eof = true;
            return 0;
        }
        const auto nBytesRead = std::min( nMaxBytesToRead, m_data.size() - m_position );
        std::memcpy( buffer, m_data.data() + m_position, nBytesRead );
        m_position += nBytesRead;
        return nBytesRead;
    }

    size_t seek( long long int, int ) override
    {
        throw std::logic_error( "NonSeekableFileReader cannot seek!" );
    }

    [[nodiscard]] std::optional<size_t> size() const override { return std::nullopt; }
    [[nodiscard]] size_t tell() const override { return m_position; }
    void clearerr() override { m_eof = false; }

private:
    std::vector<char> m_data;
    size_t m_position{ 0 };
    bool m_eof{ false };
    bool m_closed{ false };
};


void
testNonSeekableCompleteStream( const std::string& encodedTestFilePath,
                               const std::string& decodedTestFilePath )
{
    BZ2Reader reader( std::make_unique<NonSeekableFileReader>( readFile<std::vector<char> >( encodedTestFilePath ) ) );

    std::vector<char> decoded;
    std::array<char, 4_Ki> buffer{};
    while ( true ) {
        const auto nBytesRead = reader.read( -1, buffer.data(), buffer.size() );
        if ( nBytesRead == 0 ) {
            break;
        }
        decoded.insert( decoded.end(), buffer.data(), buffer.data() + nBytesRead );
    }

    REQUIRE( decoded == readFile<std::vector<char> >( decodedTestFilePath ) );
}


void
createUniformByteFile( const std::string& path,
                       const size_t       size )
{
    std::ofstream file( path, std::ios_base::out | std::ios_base::binary );
    std::mt19937 engine( 0x5EED1234U );
    std::array<char, 4_Ki> buffer{};
    for ( size_t nBytesWritten = 0; nBytesWritten < size; ) {
        for ( auto& value : buffer ) {
            value = static_cast<char>( engine() & 0xFFU );
        }
        const auto nBytesToWrite = std::min( buffer.size(), size - nBytesWritten );
        file.write( buffer.data(), static_cast<std::streamsize>( nBytesToWrite ) );
        nBytesWritten += nBytesToWrite;
    }
    REQUIRE( file.good() );
}


void
testAllSymbolKernels( const std::string& decodedPath,
                      const std::string& encodedPath )
{
    const auto expected = readFile<std::vector<char> >( decodedPath );

    std::vector<bzip2::SymbolKernel> kernels{ bzip2::SymbolKernel::PORTABLE };
    if ( bzip2::isSupported( bzip2::SymbolKernel::SSE41 ) ) {
        kernels.emplace_back( bzip2::SymbolKernel::SSE41 );
    }
    if ( bzip2::isSupported( bzip2::SymbolKernel::AVX2 ) ) {
        kernels.emplace_back( bzip2::SymbolKernel::AVX2 );
    }

    for ( const auto kernel : kernels ) {
        bzip2::setSymbolKernel( kernel );
        BZ2Reader reader( std::make_unique<StandardFileReader>( encodedPath ) );
        std::vector<char> decoded;
        std::array<char, 4_Ki> buffer{};
        while ( true ) {
            const auto nBytesRead = reader.read( -1, buffer.data(), buffer.size() );
            if ( nBytesRead == 0 ) {
                break;
            }
            decoded.insert( decoded.end(), buffer.data(), buffer.data() + nBytesRead );
        }
        REQUIRE( decoded == expected );
    }
    bzip2::setSymbolKernel( bzip2::SymbolKernel::AUTOMATIC );
}


void
testAllByteValuesAllSymbolKernels( const std::string& decodedPath,
                                   const std::string& encodedPath )
{
    const auto expected = readFile<std::vector<char> >( decodedPath );
    std::array<bool, 256> seen{};
    for ( const auto value : expected ) {
        seen[static_cast<uint8_t>( value )] = true;
    }
    /* A small alphabet cannot reach deep Move-to-Front positions >= 32 in the SIMD kernels. */
    REQUIRE( std::find( seen.begin(), seen.end(), false ) == seen.end() );

    testAllSymbolKernels( decodedPath, encodedPath );
}


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
testIncompleteIndexIsRejected( const std::string& encodedTestFilePath )
{
    const auto complete =
        std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) )->blockOffsets();
    REQUIRE( complete.size() >= 3 );

    const auto expectRejected =
        [&] ( const std::map<size_t, size_t>& offsets )
        {
            BZ2Reader reader( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
            bool threw{ false };
            try {
                reader.setBlockOffsets( offsets );
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
testConcatenatedStreams( const std::string& decodedTestFilePath,
                         const std::string& encodedTestFilePath )
{
    const auto encoded = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    auto concatenated = encoded;
    concatenated.insert( concatenated.end(), encoded.begin(), encoded.end() );

    BZ2Reader reader( std::make_unique<BufferViewFileReader>( concatenated ) );
    std::vector<char> decoded;
    std::array<char, 64_Ki> buffer{};
    while ( true ) {
        const auto nBytesRead = reader.read( buffer.data(), buffer.size() );
        if ( nBytesRead == 0 ) {
            break;
        }
        decoded.insert( decoded.end(), buffer.data(), buffer.data() + nBytesRead );
    }

    REQUIRE( reader.eof() );
    REQUIRE( reader.blockOffsetsComplete() );
    REQUIRE( reader.size().has_value() );
    REQUIRE_EQUAL( reader.size().value(), 2 * expected.size() );
    REQUIRE_EQUAL( decoded.size(), 2 * expected.size() );
    REQUIRE( std::equal( expected.begin(), expected.end(), decoded.begin() ) );
    REQUIRE( std::equal( expected.begin(), expected.end(),
                         decoded.begin() + static_cast<std::ptrdiff_t>( expected.size() ) ) );

    const auto singleOffsets =
        std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( encoded ) )->blockOffsets();
    REQUIRE( reader.blockOffsets().size() > singleOffsets.size() );
}


void
testLastToFirstMappingKernels()
{
    /* Deterministic run records, including runs longer than the 8- and 16-entry vector stores. */
    std::vector<uint8_t> records;
    size_t totalSize = 0;
    for ( size_t i = 0; i < 4000 && totalSize < 800000; ++i ) {
        const auto count = static_cast<uint8_t>( 1 + ( i * 13 ) % 255 );
        records.push_back( static_cast<uint8_t>( ( i * 37 ) % 256 ) );
        records.push_back( count );
        totalSize += count;
    }
    const auto recordCount = records.size() / 2;

    const auto reference =
        [&] ( const auto& function )
        {
            std::vector<uint32_t> mapping( bzip2::LAST_TO_FIRST_MAPPING_SIZE );
            std::array<uint32_t, 256> byteCounts{};
            const auto size = function( records.data(), recordCount, mapping.data(), byteCounts );
            return std::make_tuple( size, mapping, byteCounts );
        };

    const auto referenceResult = reference( bzip2::createLastToFirstMappingPortable );
    const auto referenceSize = std::get<0>( referenceResult );
    const auto& referenceMapping = std::get<1>( referenceResult );
    const auto& referenceCounts = std::get<2>( referenceResult );
    REQUIRE_EQUAL( referenceSize, totalSize );

    const auto compare =
        [&] ( const auto& function )
        {
            std::vector<uint32_t> mapping( bzip2::LAST_TO_FIRST_MAPPING_SIZE );
            std::array<uint32_t, 256> byteCounts{};
            const auto size = function( records.data(), recordCount, mapping.data(), byteCounts );
            REQUIRE_EQUAL( size, referenceSize );
            REQUIRE( byteCounts == referenceCounts );
            REQUIRE( std::equal( mapping.begin(), mapping.begin() + size, referenceMapping.begin() ) );
        };

#if LIBRAPIDARCHIVE_BZIP2_X86_KERNELS
    compare( bzip2::createLastToFirstMappingSSE2 );
    if ( __builtin_cpu_supports( "avx2" ) ) {
        compare( bzip2::createLastToFirstMappingAVX2 );
    }
    if ( __builtin_cpu_supports( "avx512f" ) ) {
        compare( bzip2::createLastToFirstMappingAVX512 );
    }
#endif
#if LIBRAPIDARCHIVE_BZIP2_NEON_KERNELS
    compare( bzip2::createLastToFirstMappingNEON );
#endif
}


void
testImportedIndexVerifiesStreamCRC()
{
    const auto& compressed = bzip2_test::SINGLE_BYTE_COMPRESSED_STREAM;
    const auto offsets =
        std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( compressed ) )->blockOffsets();
    REQUIRE( offsets.size() >= 3 );

    auto corrupted = compressed;
    bzip2_test::corruptStreamCRC( corrupted );

    /* With an imported index, the reader still decodes every block from the beginning and therefore must
     * verify the stream CRC instead of disabling verification because the block map is finalized. */
    BZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ) );
    reader.setBlockOffsets( offsets );
    const auto message = bzip2_test::readToEndAndCaptureMessage( reader );
    bzip2_test::requireStreamCRCError( message );
}


void
testSingleBlockCRCs()
{
    bzip2_test::checkSingleBlockCRCs(
        [] ( const std::vector<uint8_t>& data )
        {
            return std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( data ) );
        } );
}


void
testMultiBlockCRCVerification( const std::string& decodedTestFilePath,
                               const std::string& encodedTestFilePath )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );
    bzip2_test::checkMultiBlockCRCs(
        compressed, expected,
        [] ( const std::vector<uint8_t>& data )
        {
            return std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( data ) );
        } );
}


void
testImportedOffsetsVerifyStreamCRC( const std::string& decodedTestFilePath,
                                    const std::string& encodedTestFilePath )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    const auto offsets =
        std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( compressed ) )->blockOffsets();
    REQUIRE( offsets.size() >= 4 );

    /* A valid multi-block stream must still decode when the block offsets are imported. */
    {
        BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
        reader.setBlockOffsets( offsets );
        REQUIRE( bzip2_test::readAll( reader ) == expected );
    }

    /* Imported offsets must not disable the stream CRC check for a stream read from the beginning. */
    auto corrupted = compressed;
    bzip2_test::corruptStreamCRC( corrupted );
    BZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ) );
    reader.setBlockOffsets( offsets );
    const auto message = bzip2_test::readToEndAndCaptureMessage( reader );
    bzip2_test::requireStreamCRCError( message );
}


void
testImportedDecodedSizesAreValidated( const std::string& encodedTestFilePath )
{
    std::map<size_t, size_t> offsets;
    {
        BZ2Reader reader( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
        offsets = reader.blockOffsets();
    }
    REQUIRE( offsets.size() >= 3 );

    /* Shift every decoded offset after the first block by one byte. The compressed offsets still form a valid
     * map, so only comparing the claimed size against the decoded block can reject it. */
    for ( auto it = std::next( offsets.begin() ); it != offsets.end(); ++it ) {
        REQUIRE( it->second > 0 );
        it->second -= 1;
    }

    BZ2Reader reader( std::make_unique<StandardFileReader>( encodedTestFilePath ) );
    reader.setBlockOffsets( offsets );

    std::string message;
    try {
        reader.seek( 1 );
        static_cast<void>( bzip2_test::readAll( reader ) );
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "decoded block size" ) != std::string::npos );
}


void
testExactSizeReadNeedsExtraReadForStreamCRC()
{
    /* The serial reader parses the EOS block, and therefore verifies the stream CRC, only on the read after
     * the last data byte. Reading exactly size() bytes verifies every block CRC but not the stream CRC. */
    auto corrupted = bzip2_test::SINGLE_BYTE_COMPRESSED_STREAM;
    bzip2_test::corruptStreamCRC( corrupted );

    BZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ) );
    std::array<char, 1> buffer{};
    REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), size_t( 1 ) );
    REQUIRE_EQUAL( buffer[0], 'A' );

    std::string message;
    try {
        static_cast<void>( reader.read( buffer.data(), buffer.size() ) );
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    bzip2_test::requireStreamCRCError( message );
}


void
testStreamCRCVerificationAfterSeekToStart( const std::string& decodedTestFilePath,
                                           const std::string& encodedTestFilePath )
{
    const auto compressed = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    const auto offsets =
        std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( compressed ) )->blockOffsets();
    REQUIRE( offsets.size() >= 4 );

    /* Import the offsets so that seek(0) does not first read the whole stream to finalize the block map,
     * which would verify the stream CRC before the seek state is reset. */
    auto corrupted = compressed;
    bzip2_test::corruptStreamCRC( corrupted );
    {
        BZ2Reader reader( std::make_unique<BufferViewFileReader>( corrupted ) );
        reader.setBlockOffsets( offsets );
        /* Cross the first block boundary so that the stream CRC accumulator is non-zero before the seek. */
        const auto firstBlockSize = std::next( offsets.begin() )->second;
        REQUIRE( firstBlockSize > 0 );
        std::vector<char> prefix( firstBlockSize + 128 );
        REQUIRE_EQUAL( reader.read( prefix.data(), prefix.size() ), prefix.size() );
        REQUIRE_EQUAL( reader.seek( 0 ), 0U );
        const auto message = bzip2_test::readToEndAndCaptureMessage( reader );
        bzip2_test::requireStreamCRCError( message );
    }

    /* Seeking back to the start must not make a valid stream fail the stream CRC check. */
    {
        BZ2Reader reader( std::make_unique<BufferViewFileReader>( compressed ) );
        reader.setBlockOffsets( offsets );
        /* Cross the first block boundary so that the stream CRC accumulator is non-zero before the seek. */
        const auto firstBlockSize = std::next( offsets.begin() )->second;
        REQUIRE( firstBlockSize > 0 );
        std::vector<char> prefix( firstBlockSize + 128 );
        REQUIRE_EQUAL( reader.read( prefix.data(), prefix.size() ), prefix.size() );
        REQUIRE_EQUAL( reader.seek( 0 ), 0U );
        REQUIRE( bzip2_test::readAll( reader ) == expected );
    }
}


void
testConcatenatedStreamCRCVerification( const std::string& decodedTestFilePath,
                                       const std::string& encodedTestFilePath )
{
    const auto encoded = readFile<std::vector<uint8_t> >( encodedTestFilePath );
    const auto expected = readFile<std::vector<char> >( decodedTestFilePath );

    auto concatenated = encoded;
    concatenated.insert( concatenated.end(), encoded.begin(), encoded.end() );

    /* corruptStreamCRC flips the last EOS, which belongs to the second stream. */
    bzip2_test::corruptStreamCRC( concatenated );

    BZ2Reader reader( std::make_unique<BufferViewFileReader>( concatenated ) );
    std::vector<char> firstStream( expected.size() );
    size_t nBytesRead = 0;
    while ( nBytesRead < firstStream.size() ) {
        const auto nBytesReadNow = reader.read( firstStream.data() + nBytesRead, firstStream.size() - nBytesRead );
        if ( nBytesReadNow == 0 ) {
            break;
        }
        nBytesRead += nBytesReadNow;
    }
    REQUIRE_EQUAL( nBytesRead, firstStream.size() );
    REQUIRE( firstStream == expected );

    const auto message = bzip2_test::readToEndAndCaptureMessage( reader );
    bzip2_test::requireStreamCRCError( message );
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
    testSingleBlockCRCs();
    testExactSizeReadNeedsExtraReadForStreamCRC();
    testImportedIndexVerifiesStreamCRC();
    testLastToFirstMappingKernels();
    testOversizedCodeLength();

    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testBZ2Reader" );

    const auto decodedTestFilePath = tmpFolder.path() / "decoded";
    createRandomTextFile( decodedTestFilePath.string(), 2_Mi );

    const auto encodedTestFilePath = tmpFolder.path() / "encoded-sample.bz2";
    bzip2_test::compressFile( decodedTestFilePath, encodedTestFilePath );

    const auto zerosDecodedPath = tmpFolder.path() / "zeros";
    {
        std::ofstream zerosFile( zerosDecodedPath, std::ios_base::out | std::ios_base::binary );
        const std::array<char, 4_Ki> zeros{};
        for ( size_t i = 0; i < 512; ++i ) {
            zerosFile.write( zeros.data(), zeros.size() );
        }
    }
    const auto zerosEncodedPath = tmpFolder.path() / "zeros.bz2";
    bzip2_test::compressFile( zerosDecodedPath, zerosEncodedPath );

    const auto uniformDecodedPath = tmpFolder.path() / "uniform-bytes";
    createUniformByteFile( uniformDecodedPath.string(), 2_Mi );
    const auto uniformEncodedPath = tmpFolder.path() / "uniform-bytes.bz2";
    bzip2_test::compressFile( uniformDecodedPath, uniformEncodedPath );

    try
    {
        testSimpleOpenAndClose( encodedTestFilePath.string() );
        testNonSeekableCompleteStream( encodedTestFilePath.string(), decodedTestFilePath.string() );
        testAllSymbolKernels( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testAllSymbolKernels( zerosDecodedPath.string(), zerosEncodedPath.string() );
        testAllByteValuesAllSymbolKernels( uniformDecodedPath.string(), uniformEncodedPath.string() );

        testMultiBlockCRCVerification( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testImportedOffsetsVerifyStreamCRC( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testImportedDecodedSizesAreValidated( encodedTestFilePath.string() );
        testStreamCRCVerificationAfterSeekToStart( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testConcatenatedStreamCRCVerification( decodedTestFilePath.string(), encodedTestFilePath.string() );

        testDecodingBz2ForFirstTime( decodedTestFilePath.string(), encodedTestFilePath.string() );

        /* This test works because any seeking back triggers the completion of the block offset map! */
        testSeekBeforeOffsetCompletion( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testIndexImportExport( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testConcatenatedStreams( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testIncompleteIndexIsRejected( encodedTestFilePath.string() );
    } catch ( const std::exception& exception ) {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
