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

#include "Bzip2ReaderTests.hpp"
#include "Bzip2TestHelpers.hpp"


using namespace rapidgzip;
using namespace indexed_bzip2;
using namespace bzip2_test;


struct SerialReaderPolicy
{
    using Reader = BZ2Reader;

    static std::unique_ptr<Reader>
    make( std::unique_ptr<FileReader> file,
          size_t )
    {
        return std::make_unique<Reader>( std::move( file ) );
    }

    static void
    joinThreads( Reader& )
    {}
};


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

    uintmax_t seek( long long int, int ) override
    {
        throw std::logic_error( "NonSeekableFileReader cannot seek!" );
    }

    [[nodiscard]] std::optional<uintmax_t> size() const override { return std::nullopt; }
    [[nodiscard]] uintmax_t tell() const override { return m_position; }
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
testNonSeekableEmptyInput()
{
    /* Non-seekable readers only report EOF after a failed read. The first read must still report an empty
     * archive instead of throwing from the stream header parsing. */
    BZ2Reader reader( std::make_unique<NonSeekableFileReader>( std::vector<char>{} ) );
    std::array<char, 16> buffer{};
    REQUIRE_EQUAL( reader.read( buffer.data(), buffer.size() ), 0U );
    REQUIRE( reader.eof() );
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

    for ( const auto kernel : bzip2_test::supportedSymbolKernels() ) {
        const bzip2_test::ScopedSymbolKernel kernelScope{ kernel };
        BZ2Reader reader( std::make_unique<StandardFileReader>( encodedPath ) );
        REQUIRE( bzip2_test::readAll( reader ) == expected );
    }
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
testSeekBeforeOffsetCompletion( const std::string& decodedTestFilePath,
                                const std::string& encodedTestFilePath )
{
    const auto decodedFileSize = fileSize( decodedTestFilePath );
    std::cerr << "Decoded file size: " << decodedFileSize << "\n";

    const auto blockOffsets =
        std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) )->blockOffsets();

    std::ifstream decodedFile( decodedTestFilePath, std::ios_base::in | std::ios_base::binary );
    auto encodedFile = std::make_unique<BZ2Reader>( std::make_unique<StandardFileReader>( encodedTestFilePath ) );

    const auto seek =
        [&] ( long long int offset,
              int           origin = SEEK_SET )
        { bzip2_test::testSeek( decodedFileSize, decodedFile, encodedFile, offset, origin ); };

    const auto read =
        [&] ( size_t nBytesToRead )
        { bzip2_test::testRead( decodedFile, encodedFile, nBytesToRead ); };

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

#if LIBRAPIDARCHIVE_COMPILE_X86_64_SIMD_KERNELS
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

    bzip2_test::requireStreamCRCError( bzip2_test::readToEndAndCaptureMessage( reader ) );
}


int
main()
{
    testNonSeekableEmptyInput();
    testLastToFirstMappingKernels();
    testExactSizeReadNeedsExtraReadForStreamCRC();

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

    constexpr auto parallelization = size_t{ 0 };

    try
    {
        testSimpleOpenAndClose<SerialReaderPolicy>( encodedTestFilePath.string(), parallelization );
        testNonSeekableCompleteStream( encodedTestFilePath.string(), decodedTestFilePath.string() );
        testAllSymbolKernels( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testAllSymbolKernels( zerosDecodedPath.string(), zerosEncodedPath.string() );
        testAllByteValuesAllSymbolKernels( uniformDecodedPath.string(), uniformEncodedPath.string() );

        testEmptyAndTruncatedInputs<SerialReaderPolicy>( encodedTestFilePath.string(), parallelization );
        testRunLengthOverflow<SerialReaderPolicy>( parallelization );
        testOversizedCodeLength<SerialReaderPolicy>( parallelization );
        testInvalidBlockOffsetsLeaveReaderUsable<SerialReaderPolicy>( encodedTestFilePath.string(),
                                                                      parallelization );
        testSingleBlockCRCs<SerialReaderPolicy>( parallelization );
        testMultiBlockCRCVerification<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
        testImportedIndexVerifiesStreamCRC<SerialReaderPolicy>( parallelization );
        testImportedOffsetsVerifyStreamCRC<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
        testImportedDecodedSizesAreValidated<SerialReaderPolicy>( encodedTestFilePath.string(), parallelization );
        testStreamCRCVerificationAfterSeekToStart<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
        testConcatenatedStreamCRCVerification<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
        testRejectsIndexForFirstConcatenatedStream<SerialReaderPolicy>(
            encodedTestFilePath.string(), parallelization );

        testDecodingBz2ForFirstTime<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );

        /* This test works because any seeking back triggers the completion of the block offset map! */
        testSeekBeforeOffsetCompletion( decodedTestFilePath.string(), encodedTestFilePath.string() );
        testIndexImportExport<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
        testIncompleteIndexIsRejected<SerialReaderPolicy>( encodedTestFilePath.string(), parallelization );
        testConcatenatedStreams<SerialReaderPolicy>(
            decodedTestFilePath.string(), encodedTestFilePath.string(), parallelization );
    } catch ( const std::exception& exception ) {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
