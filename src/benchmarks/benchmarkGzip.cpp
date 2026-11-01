/**
 * @file Executes benchmarks for varying gzip decompressors (zlib, libarchive, rapidgzip: sequential, parallel,
 *       with index, with different chunk sizes) for a given compressed file.
 *       It is to be used as an integration benchmark especially for the parallel decompressor and for comparison
 *       how much it improves over preexisting solutions like zlib. Furthermore, it compares the result size
 *       of all implementations to check for errors.
 */

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <archive.h>
#include <zlib.h>

#include <core/common.hpp>
#include <core/FileUtils.hpp>
#include <core/Statistics.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Standard.hpp>
#include <rapidgzip/gzip/GzipReader.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>


using namespace rapidgzip;


/* Unfortunately, this is yet another performance-critical parameter, which is not directly related to the
 * decompression algorithm implementation :(. There is trade-off between staying in cache, I think, and the
 * number of decompression calls. Some tests with:
 *   cmake -DLIBRAPIDARCHIVE_WITH_ISAL=OFF -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=ON ..
 *   cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip silesia-20x.tar.gz
 *
 * Decompressed 1364784177 B to 4239155200 B with libarchive:
 *   512 MiB : 394.4 <= 395.6 +- 1.2 <= 396.8
 *   256 MiB : 402.5 <= 404.8 +- 2.2 <= 407.0
 *   128 MiB : 408.3 <= 410.7 +- 2.5 <= 413.3
 *    64 MiB : 406   <= 412   +- 5   <= 416    <- Local top, but the 1-sigma error bounds overlap its neighbors.
 *    32 MiB : 403   <= 408   +- 4   <= 411
 *    16 MiB : 403   <= 408   +- 5   <= 414
 *     4 MiB : 418   <= 421   +- 3   <= 424
 *     1 MiB : 415   <= 420   +- 6   <= 426    <- Multiple local top, but only 1-sigma.
 *   256 KiB : 409   <= 418   +- 8   <= 423
 *    64 KiB : 422.5 <= 425.4 +- 2.7 <= 427.8
 *     8 KiB : 417.9 <= 420.9 +- 2.9 <= 423.7  <- Multiple local top, but only 1-sigma.
 *     1 KiB : 414   <= 417   +- 5   <= 422
 *   128   B : 413   <= 417   +- 5   <= 422
 *     8   B : 319   <= 323   +- 4   <= 327    <- FINALLY some actual slowdown!
 *     2   B : 192.1 <= 194.5 +- 2.6 <= 197.2  <- How has this actually usable speeds?!
 * -> It is eerie how few fluctations there are over these magnitudes of output buffer sizes.
 *    Debug output for the number of bytes returned by archive_read_data show that it is indeed always
 *    decoding one full buffer per call! It seems that there simply is a lot of inlining going on
 *    so that the call overhead is almost negligible.
 *
 * These benchmarks used the default chunk size of 4 MiB for ParallelGzipReader.
 *
 * Decompressed 1364784177 B to 4239155200 B with rapidgzip (serial):
 *   512 MiB : 326.9 <= 329.0 +- 1.8 <= 330.0
 *   256 MiB : 327.8 <= 328.3 +- 0.4 <= 328.5
 *   128 MiB : 332   <= 336   +- 4   <= 339
 *    64 MiB : 327.1 <= 329.2 +- 1.9 <= 330.9
 *    32 MiB : 326   <= 336   +- 9   <= 344
 *    16 MiB : 330.2 <= 331.7 +- 2.3 <= 334.4
 *     8 MiB : 327.7 <= 330.7 +- 2.6 <= 332.5
 *     4 MiB : 327.5 <= 330.4 +- 3.0 <= 333.5
 *     1 MiB : 340.3 <= 340.9 +- 1.1 <= 342.2  <- local top
 *   256 KiB : 335   <= 342   +- 6   <= 346    <- local top
 *   128 KiB : 336.4 <= 338.3 +- 1.9 <= 340.1
 *    64 KiB : 344.9 <= 346.8 +- 1.7 <= 348.2  <- local top, but only barely.
 *    32 KiB : 346.3 <= 347.8 +- 1.5 <= 349.3
 *     8 KiB : 338.9 <= 339.7 +- 1.4 <= 341.4
 *     1 KiB : 334.5 <= 335.3 +- 0.7 <= 336.0
 *   128   B : 310.9 <= 313.5 +- 2.4 <= 315.6
 *     8   B : 128.8 <= 131.0 +- 1.9 <= 132.3
 *  -> Also stable along a very large range! Unfortunately, not exposed via Python any longer.
 *
 * Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 1 cores):
 *   512 MiB : 295   <= 298   +- 3   <= 301
 *   256 MiB : 286   <= 291   +- 5   <= 295
 *   128 MiB : 295.3 <= 296.7 +- 1.5 <= 298.4
 *    64 MiB : 298.8 <= 300.1 +- 2.0 <= 302.4
 *    32 MiB : 295   <= 298   +- 4   <= 302
 *    16 MiB : 300.3 <= 303.3 +- 2.7 <= 305.5
 *     8 MiB : 298   <= 302   +- 4   <= 306
 *     4 MiB : 297.6 <= 300.5 +- 2.5 <= 302.1
 *     1 MiB : 307.9 <= 309.3 +- 2.0 <= 311.6
 *   256 KiB : 311.0 <= 312.5 +- 1.4 <= 313.5  <- local top
 *   128 KiB : 304   <= 309   +- 5   <= 314
 *    64 KiB : 304.5 <= 306.6 +- 1.9 <= 307.7
 *    32 KiB : 298   <= 305   +- 7   <= 312
 *     8 KiB : 307.9 <= 311.0 +- 2.7 <= 313.0
 *     1 KiB : 283.8 <= 285.5 +- 1.6 <= 287.0
 *   128   B : 173.0 <= 173.4 +- 0.5 <= 174.0
 *    32   B :  70.4 <=  70.9 +- 0.5 <=  71.3
 * -> 10% slower than GzipReader for some reason!? Just because of the ChunkMap lookup, std::launch(std::deferred),
 *    and std::future evaluation?! But, I guess it is still quite a good result for the much more complex architecture.
 *
 * Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 24 cores):
 *   512 MiB : 1980 <= 1996 +-  27 <= 2027
 *   256 MiB : 2030 <= 2130 +- 110 <= 2250  <- still fine range
 *   128 MiB : 2337 <= 2343 +-   6 <= 2350  <- still fine range
 *    64 MiB : 2290 <= 2330 +-  30 <= 2360  <- still fine range
 *    32 MiB : 2409 <= 2429 +-  25 <= 2458  <- ideal range
 *    16 MiB : 2393 <= 2415 +-  20 <= 2431  <- ideal range
 *     8 MiB : 2350 <= 2390 +-  30 <= 2410  <- ideal range
 *     4 MiB : 1993 <= 2016 +-  21 <= 2034  <- begins to slow
 *     1 MiB : 1950 <= 2000 +-  40 <= 2030  <- begins to slow
 *   256 KiB : 1970 <= 2040 +-  60 <= 2070  <- begins to slow
 *   128 KiB : 1980 <= 2010 +-  40 <= 2050  <- begins to slow
 *    64 KiB : 1906 <= 1932 +-  25 <= 1954  <- begins to slow
 *    32 KiB : 1977 <= 1997 +-  17 <= 2010  <- begins to slow
 *     8 KiB : 1926 <= 1939 +-  21 <= 1963  <- begins to slow
 *     1 KiB : 1475 <= 1493 +-  17 <= 1510
 *   128   B :  348 <= 350  +-   3 <=  354
 *    32   B : 90.4 <= 90.7 +- 0.4 <= 91.2
 *  -> This is more according to my expectations. It cannot inline these calls and the call overhead,
 *     especially for finding the chunk and possibly waiting on mutexes, is expected to be expensive
 *     and degrades performance!
 *
 * Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel + index):
 *   512 MiB : 2830 <= 2880 +-  60 <= 2950  <- only half speed
 *   256 MiB : 3357 <= 3369 +-  11 <= 3378  <- begins to slow
 *   128 MiB : 3614 <= 3643 +-  25 <= 3659  <- begins to slow
 *    64 MiB : 3710 <= 3830 +- 170 <= 4020  <- begins to slow
 *    32 MiB : 3890 <= 3990 +- 100 <= 4100  <- begins to slow
 *    16 MiB : 4157 <= 4168 + - 11 <= 4177  <- begins to slow
 *     8 MiB : 4740 <= 4790 +-  60 <= 4850  <- still fine range
 *     4 MiB : 4710 <= 4900 +- 170 <= 5030  <- ideal range
 *     1 MiB : 4870 <= 5060 +- 160 <= 5190  <- ideal range
 *   256 KiB : 5130 <= 5210 +-  80 <= 5290  <- ideal range
 *   128 KiB : 4980 <= 5050 +- 100 <= 5170  <- ideal range
 *    64 KiB : 4930 <= 5050 +- 110 <= 5150  <- ideal range
 *    32 KiB : 4850 <= 5020 +- 150 <= 5120  <- ideal range
 *     8 KiB : 4570 <= 4630 +-  60 <= 4690  <- still fine range
 *     1 KiB : 1880 <= 1920 +-  40 <= 1950  <- only half speed
 *   128   B :  381 <=  386 +-   5 <=  391
 *    32   B : 95.0 <= 95.4 +- 0.4 <= 95.7
 * -> It is interesting to see the 128 B case being equally fast no matter the cores.
 *    This shows that the bottleneck is probably the 'read'-method call overhead, not the decompression.
 *    Perf record shows most of the time as being spent in: deflate::Block::read, so not very helpful.
 * -> For 8 B, perf shows most of the time (21 %) spent in
 *      rapidgzip::ParallelGzipReader<rapidgzip::ChunkData>::read(
 *          int, char*, unsigned long)::{
 *              lambda(std::shared_ptr<rapidgzip::ChunkData> const&, unsigned long, unsigned long)#1
 *          }::operator()(std::shared_ptr<rapidgzip::ChunkData> const&, unsigned long, unsigned long)
 *    So, the time is spent inside the 'writeFunctor' and probably function calling overhead because
 *    it is unclear where the remaining 80 % is spent.
 *    8 B takes too long to take timings. So probably slower than 10 MB/s!
 */
constexpr size_t DECOMPRESSION_BUFFER_SIZE = 8_Mi;

class GzipWrapper
{
public:
    static constexpr auto WINDOW_SIZE = 32_Ki;

    enum class Format
    {
        AUTO,
        RAW,
        GZIP,
    };

public:
    explicit
    GzipWrapper( Format format = Format::AUTO ) :
        m_format( format )
    {
        init( format );
    }

    /* These are implicitly deleted anyway because of the const member but clang-tidy keeps complaining without this. */
    GzipWrapper( const GzipWrapper& ) = delete;

    GzipWrapper( GzipWrapper&& ) = delete;

    GzipWrapper&
    operator=( GzipWrapper&& ) = delete;

    GzipWrapper&
    operator=( GzipWrapper& ) = delete;

private:
    void
    init( Format format )
    {
        m_stream = {};

        m_stream.zalloc = Z_NULL;     /* used to allocate the internal state */
        m_stream.zfree = Z_NULL;      /* used to free the internal state */
        m_stream.opaque = Z_NULL;     /* private data object passed to zalloc and zfree */

        m_stream.avail_in = 0;        /* number of bytes available at next_in */
        m_stream.next_in = Z_NULL;    /* next input byte */

        m_stream.avail_out = 0;       /* remaining free space at next_out */
        m_stream.next_out = Z_NULL;   /* next output byte will go here */

        m_stream.msg = nullptr;

        int windowBits = 15;  // maximum value corresponding to 32kiB;
        switch ( format )
        {
        case Format::AUTO:
            windowBits += 32;
            break;

        case Format::RAW:
            windowBits *= -1;
            break;

        case Format::GZIP:
            windowBits += 16;
            break;
        }

        auto ret = inflateInit2( &m_stream, windowBits );
        if ( ret != Z_OK ) {
            throw std::runtime_error( "inflateInit2 returned error code: " + std::to_string( ret ) );
        }
    }

public:
    ~GzipWrapper()
    {
        inflateEnd( &m_stream );
    }

    [[nodiscard]] size_t
    inflate( unsigned char const** compressedData,
             size_t*               compressedSize )
    {
        if ( *compressedSize == 0 ) {
            return 0;
        }

        m_stream.avail_in = *compressedSize;
        /* const_cast should be safe because zlib presumably only uses this in a const manner. */
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        m_stream.next_in = const_cast<unsigned char*>( *compressedData );

        m_stream.avail_out = m_outputBuffer.size();
        m_stream.next_out = m_outputBuffer.data();

        /* When using Z_FINISH, it seems that avail_in and next_in are not updated!
         * Plus, the output buffer must be large enough to hold everything. Use Z_NO_FLUSH instead. */
        const auto errorCode = ::inflate( &m_stream, Z_NO_FLUSH );
        *compressedData = m_stream.next_in;
        *compressedSize = m_stream.avail_in;
        if ( ( errorCode != Z_OK ) && ( errorCode != Z_STREAM_END ) ) {
            return 0;
        }

        if ( m_stream.avail_out > m_outputBuffer.size() ) {
            throw std::logic_error( "Zlib returned and invalid value!" );
        }

        const auto nBytesDecoded = m_outputBuffer.size() - m_stream.avail_out;

        if ( errorCode == Z_STREAM_END ) {
            /* Reinitialize internal data at end position to support multi-stream input. */
            if ( *compressedSize > 0 ) {
                inflateEnd( &m_stream );
                init( m_format );
            }
        }

        return nBytesDecoded;
    }

private:
    const Format m_format;
    z_stream m_stream{};
    std::vector<unsigned char> m_window = std::vector<unsigned char>( 32_Ki, '\0' );
    std::vector<unsigned char> m_outputBuffer = std::vector<unsigned char>( DECOMPRESSION_BUFFER_SIZE );
};


[[nodiscard]] size_t
decompressWithZlib( const std::vector<uint8_t>& compressedData )
{
    GzipWrapper gzip;
    const auto* pCompressedData = compressedData.data();
    auto compressedSize = compressedData.size();
    size_t totalDecodedBytes = 0;

    while ( compressedSize > 0 )
    {
        const auto decodedBytes = gzip.inflate( &pCompressedData, &compressedSize );
        if ( decodedBytes == 0 ) {
            break;
        }
        totalDecodedBytes += decodedBytes;
    }

    return totalDecodedBytes;
}


[[nodiscard]] size_t
decompressWithLibArchive( const std::vector<uint8_t>& compressedData )
{
    auto* const pArchive = archive_read_new();
    archive_read_support_filter_gzip( pArchive );
    archive_read_support_format_raw( pArchive );
    auto errorCode = archive_read_open_memory( pArchive, compressedData.data(), compressedData.size() );
    if ( errorCode != ARCHIVE_OK ) {
        throw std::runtime_error( "Could not initialize libarchive!" );
    }

    struct archive_entry *entry{ nullptr };
    errorCode = archive_read_next_header( pArchive, &entry );
    if ( errorCode != ARCHIVE_OK ) {
        throw std::runtime_error( "Could not read header with libarchive!" );
    }

    std::vector<uint8_t> outputBuffer( DECOMPRESSION_BUFFER_SIZE );
    size_t totalDecodedBytes = 0;
    while ( true )
    {
        const auto nBytesDecoded = archive_read_data( pArchive, outputBuffer.data(), outputBuffer.size() );
        if ( nBytesDecoded < 0 ) {
            throw std::runtime_error( "Reading with libarchive failed!" );
        }
        if ( nBytesDecoded == 0 ) {
            break;
        }
        totalDecodedBytes += nBytesDecoded;
    }

    archive_read_free( pArchive );
    return totalDecodedBytes;
}


[[nodiscard]] size_t
decompressWithRapidgzip( const std::string& fileName )
{
    using namespace rapidgzip;

    size_t totalDecodedBytes = 0;
    size_t blockCount = 0;

    GzipReader gzipReader( std::make_unique<StandardFileReader>( fileName ) );
    std::vector<uint8_t> outputBuffer( DECOMPRESSION_BUFFER_SIZE );
    while ( true ) {
        const auto nBytesRead = gzipReader.read( -1,
                                                 reinterpret_cast<char*>( outputBuffer.data() ),
                                                 outputBuffer.size(),
                                                 StoppingPoint::END_OF_BLOCK_HEADER );
        if ( ( nBytesRead == 0 ) && gzipReader.eof() ) {
            break;
        }

        const auto currentPoint = gzipReader.currentPoint();
        if ( currentPoint == StoppingPoint::END_OF_BLOCK_HEADER ) {
            blockCount++;
        }
        totalDecodedBytes += nBytesRead;
    }

    std::cerr << "Decoded " << blockCount << " deflate blocks\n";

    return totalDecodedBytes;
}


[[nodiscard]] size_t
decompressWithParallelGzipReader( const std::string& fileName,
                                  const size_t       parallelization = 0,
                                  uint64_t           chunkSizeInBytes = 4_Mi )
{
    size_t totalDecodedBytes = 0;

    rapidgzip::ParallelGzipReader gzipReader(
        std::make_unique<StandardFileReader>( fileName ), parallelization, chunkSizeInBytes );
    std::vector<uint8_t> outputBuffer( DECOMPRESSION_BUFFER_SIZE );
    while ( true ) {
        const auto nBytesRead = gzipReader.read( -1,
                                                 reinterpret_cast<char*>( outputBuffer.data() ),
                                                 outputBuffer.size() );
        if ( ( nBytesRead == 0 ) && gzipReader.eof() ) {
            break;
        }

        totalDecodedBytes += nBytesRead;
    }

    return totalDecodedBytes;
}


[[nodiscard]] std::pair<std::string, GzipIndex>
createGzipIndex( const std::string& fileName )
{
    /* Create index using indexed_gzip */
    const auto indexFile = fileName + ".index";
    const auto command =
        R"(python3 -c 'import indexed_gzip as ig; f = ig.IndexedGzipFile( ")"
        + std::string( fileName )
        + R"(" ); f.build_full_index(); f.export_index( ")"
        + std::string( indexFile )
        + R"(" );')";

    const auto returnCode = std::system( command.c_str() );
    REQUIRE( returnCode == 0 );
    if ( returnCode != 0 ) {
        throw std::runtime_error( "Failed to create index using indexed_gzip Python module" );
    }

    return { fileName, readGzipIndex( std::make_unique<StandardFileReader>( indexFile ) ) };
}


[[nodiscard]] size_t
decompressWithRapidgzipParallelIndex( const std::pair<std::string, GzipIndex>& files )
{
    const auto& [fileName, index] = files;

    size_t totalDecodedBytes = 0;

    rapidgzip::ParallelGzipReader gzipReader( std::make_unique<StandardFileReader>( fileName ) );
    gzipReader.setBlockOffsets( index );
    std::vector<uint8_t> outputBuffer( DECOMPRESSION_BUFFER_SIZE );
    while ( true ) {
        const auto nBytesRead = gzipReader.read( -1,
                                                 reinterpret_cast<char*>( outputBuffer.data() ),
                                                 outputBuffer.size() );
        if ( ( nBytesRead == 0 ) && gzipReader.eof() ) {
            break;
        }

        totalDecodedBytes += nBytesRead;
    }

    return totalDecodedBytes;
}


void
printBandwidths( const std::vector<double>& durations,
                 size_t                     nBytesEncoded,
                 size_t                     nBytesDecoded )
{
    std::cout << "    Runtime / s: " << Statistics<double>( durations ).formatAverageWithUncertainty( true ) << "\n";

    std::vector<double> encodedBandwidths( durations.size() );
    std::transform( durations.begin(), durations.end(), encodedBandwidths.begin(),
                    [nBytesEncoded] ( auto duration ) {
                        return static_cast<double>( nBytesEncoded ) / 1e6 / duration;
                    } );
    std::cout << "    Bandwidth on Encoded Data / (MB/s): "
              << Statistics<double>( encodedBandwidths ).formatAverageWithUncertainty( true ) << "\n";

    std::vector<double> decodedBandwidths( durations.size() );
    std::transform( durations.begin(), durations.end(), decodedBandwidths.begin(),
                    [nBytesDecoded] ( auto duration ) {
                        return static_cast<double>( nBytesDecoded ) / 1e6 / duration;
                    } );
    std::cout << "    Bandwidth on Decoded Data / (MB/s): "
              << Statistics<double>( decodedBandwidths ).formatAverageWithUncertainty( true ) << "\n";
};
void
benchmarkChunkedParallelDecompression( const std::string& fileName )
{
    const auto fileContents = readFile<std::vector<uint8_t> >( fileName );

    std::cout << "\n== Benchmarking with rapidgzip in parallel with different decoding chunk sizes ==\n\n";

    const auto [sizeLibArchive, durationsLibArchive] = benchmarkFunction<3>(
        [&fileContents] () { return decompressWithLibArchive( fileContents ); } );
    //std::cout << "Decompressed " << fileContents.size() << " B to " << sizeLibArchive << " B with libarchive:\n";
    //printBandwidths( durationsLibArchive, fileContents.size(), sizeLibArchive );

    const auto expectedSize = sizeLibArchive;

    for ( size_t nBlocksToSkip : { 0, 1, 2, 4, 8, 16, 24, 32, 64, 128 } ) {
        const auto [sizeRapidgzipParallel, durationsRapidgzipParallel] = benchmarkFunction<3>(
            [&fileName, nBlocksToSkip] () {
                return decompressWithParallelGzipReader( fileName, nBlocksToSkip );
            } );
        if ( sizeRapidgzipParallel == expectedSize ) {
            std::cout << "Decompressed " << fileContents.size() << " B to " << sizeRapidgzipParallel << " B "
                      << "with rapidgzip (parallel, nBlocksToSkip=" << nBlocksToSkip << "):\n";
            printBandwidths( durationsRapidgzipParallel, fileContents.size(), sizeRapidgzipParallel );
        } else {
            std::cerr << "Decompressing with rapidgzip (parallel, nBlocksToSkip=" << nBlocksToSkip
                      << ") decoded a different amount (" << sizeRapidgzipParallel
                      << ") than libarchive (" << expectedSize << ")!\n";
        }
    }

    std::cout << "\n";
}


void
benchmarkDecompression( const std::string& fileName )
{
    const auto fileContents = readFile<std::vector<uint8_t> >( fileName );

    const auto [sizeLibArchive, durationsLibArchive] = benchmarkFunction<3>(
        [&fileContents] () { return decompressWithLibArchive( fileContents ); } );
    std::cout << "Decompressed " << fileContents.size() << " B to " << sizeLibArchive << " B with libarchive:\n";
    printBandwidths( durationsLibArchive, fileContents.size(), sizeLibArchive );

    const auto expectedSize = sizeLibArchive;

    const auto [sizeZlib, durationsZlib] = benchmarkFunction<3>(
        [&fileContents] () { return decompressWithZlib( fileContents ); } );
    if ( sizeZlib == expectedSize ) {
        std::cout << "Decompressed " << fileContents.size() << " B to " << sizeZlib << " B with zlib:\n";
        printBandwidths( durationsZlib, fileContents.size(), sizeZlib );
    } else {
        std::cerr << "Decompressing with zlib decoded a different amount than libarchive!\n";
    }

    const auto [sizeRapidgzip, durationsRapidgzip] = benchmarkFunction<3>(
        [&fileName] () { return decompressWithRapidgzip( fileName ); } );
    if ( sizeRapidgzip == expectedSize ) {
        std::cout << "Decompressed " << fileContents.size() << " B to " << sizeRapidgzip << " B "
                  << "with rapidgzip (serial):\n";
        printBandwidths( durationsRapidgzip, fileContents.size(), sizeRapidgzip );
    } else {
        std::cerr << "Decompressing with rapidgzip (serial) decoded a different amount than libarchive!\n";
    }

    const std::vector<size_t> parallelizations = { 1, availableCores() };
    for ( const auto parallelization : parallelizations ) {
        const auto [sizeRapidgzipParallel, durationsRapidgzipParallel] = benchmarkFunction<3>(
            [&fileName, parallelization = parallelization] () {
                return decompressWithParallelGzipReader( fileName, parallelization );
            } );
        if ( sizeRapidgzipParallel == expectedSize ) {
            std::cout << "Decompressed " << fileContents.size() << " B to " << sizeRapidgzipParallel << " B "
                      << "with rapidgzip (parallel: " << parallelization << " cores):\n";
            printBandwidths( durationsRapidgzipParallel, fileContents.size(), sizeRapidgzipParallel );
        } else {
            throw std::logic_error( "Decompressing with rapidgzip (parallel) decoded a different amount ("
                                    + std::to_string( sizeRapidgzipParallel ) + ") than libarchive ("
                                    + std::to_string( expectedSize ) + ")!" );
        }
    }

    const auto [sizeRapidgzipParallelIndex, durationsRapidgzipParallelIndex] = benchmarkFunction<3>(
        [&fileName] () { return createGzipIndex( fileName ); }, decompressWithRapidgzipParallelIndex );
    if ( sizeRapidgzipParallelIndex == expectedSize ) {
        std::cout << "Decompressed " << fileContents.size() << " B to " << sizeRapidgzipParallelIndex << " B "
                  << "with rapidgzip (parallel + index):\n";
        printBandwidths( durationsRapidgzipParallelIndex, fileContents.size(), sizeRapidgzipParallelIndex );
    } else {
        throw std::logic_error( "Decompressing with rapidgzip (parallel + index) decoded a different amount than "
                                "libarchive!" );
    }
}


int
main( int    argc,
      char** argv )
{
    if ( argc != 2 ) {
        std::cerr << "Please specify a gzip-compressed test file!\n";
        return 1;
    }

    if ( !fileExists( argv[1] ) ) {
        std::cerr << "Could not find specified file: " << argv[1] << "\n";
        return 2;
    }
    const std::string fileName = argv[1];

    if ( ( std::filesystem::path( fileName ).extension() == ".bgz" )
         || ( std::filesystem::path( fileName ).extension() == ".pigz" )
         || ( std::filesystem::path( fileName ).extension() == ".pgz" ) ) {
        benchmarkChunkedParallelDecompression( fileName );
    }

    benchmarkDecompression( fileName );

    return 0;
}


/**
mkdir -p build && cd build
base64 /dev/urandom | head -c $(( 512*1024*1024 )) > small
gzip -k small

CXX=clang++-20 CC=clang-20 cmake -DCMAKE_BUILD_TYPE=Release -DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON \
    -DLIBRAPIDARCHIVE_WITH_ISAL=ON -DLIBRAPIDARCHIVE_USE_ZLIB_NG=ON -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=OFF ..
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip small.gz

    Decompressed 407988639 B to 536870912 B with libarchive:
        Runtime / s: 1.1001 <= 1.1028 +- 0.0023 <= 1.1044
        Bandwidth on Encoded Data / (MB/s): 369.4 <= 370.0 +- 0.8 <= 370.9
        Bandwidth on Decoded Data / (MB/s): 486.1 <= 486.8 +- 1.0 <= 488.0
    Decompressed 407988639 B to 536870912 B with zlib:
        Runtime / s: 1.189 <= 1.195 +- 0.005 <= 1.199
        Bandwidth on Encoded Data / (MB/s): 340.2 <= 341.5 +- 1.5 <= 343.1
        Bandwidth on Decoded Data / (MB/s): 447.7 <= 449.3 +- 1.9 <= 451.4

    Decoded 15844 deflate blocks
    Decoded 15844 deflate blocks
    Decoded 15844 deflate blocks
    Decompressed 407988639 B to 536870912 B with rapidgzip (serial):
        Runtime / s: 2.36 <= 2.38 +- 0.03 <= 2.41
        Bandwidth on Encoded Data / (MB/s): 169.1 <= 171.7 +- 2.3 <= 173.2
        Bandwidth on Decoded Data / (MB/s): 222.5 <= 225.9 +- 3.0 <= 227.9
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 1.141 <= 1.152 +- 0.009 <= 1.160
        Bandwidth on Encoded Data / (MB/s): 351.8 <= 354.3 +- 2.9 <= 357.4
        Bandwidth on Decoded Data / (MB/s): 463 <= 466 +- 4 <= 470
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 0.139 <= 0.150 +- 0.010 <= 0.156
        Bandwidth on Encoded Data / (MB/s): 2610 <= 2720 +- 190 <= 2940
        Bandwidth on Decoded Data / (MB/s): 3440 <= 3580 +- 250 <= 3870
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel + index):
        Runtime / s: 0.110 <= 0.115 +- 0.008 <= 0.124
        Bandwidth on Encoded Data / (MB/s): 3280 <= 3560 +- 240 <= 3700
        Bandwidth on Decoded Data / (MB/s): 4300 <= 4700 +- 300 <= 4900

      -> rapidgzip (serial) is significantly slower than libarchive
         -> Decoding with rapidgzip in parallel with an index is now much faster because of internal zlib/ISA-l usage!

ISAL=OFF ZLIB_NG=ON:

    Decompressed 407988639 B to 536870912 B with libarchive:
        Runtime / s: 1.072 <= 1.081 +- 0.009 <= 1.091
        Bandwidth on Encoded Data / (MB/s): 374 <= 377 +- 3 <= 380
        Bandwidth on Decoded Data / (MB/s): 492 <= 497 +- 4 <= 501
    Decompressed 407988639 B to 536870912 B with zlib:
        Runtime / s: 1.160 <= 1.175 +- 0.018 <= 1.194
        Bandwidth on Encoded Data / (MB/s): 342 <= 347 +- 5 <= 352
        Bandwidth on Decoded Data / (MB/s): 450 <= 457 +- 7 <= 463
    Decoded 15844 deflate blocks
    Decoded 15844 deflate blocks
    Decoded 15844 deflate blocks
    Decompressed 407988639 B to 536870912 B with rapidgzip (serial):
        Runtime / s: 2.119 <= 2.124 +- 0.007 <= 2.132
        Bandwidth on Encoded Data / (MB/s): 191.4 <= 192.1 +- 0.6 <= 192.6
        Bandwidth on Decoded Data / (MB/s): 251.8 <= 252.7 +- 0.8 <= 253.4
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 2.297 <= 2.305 +- 0.009 <= 2.314
        Bandwidth on Encoded Data / (MB/s): 176.3 <= 177.0 +- 0.7 <= 177.6
        Bandwidth on Decoded Data / (MB/s): 232.0 <= 232.9 +- 0.9 <= 233.7
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 0.197 <= 0.217 +- 0.019 <= 0.235
        Bandwidth on Encoded Data / (MB/s): 1740 <= 1890 +- 170 <= 2070
        Bandwidth on Decoded Data / (MB/s): 2290 <= 2490 +- 220 <= 2720
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel + index):
        Runtime / s: 0.104 <= 0.110 +- 0.005 <= 0.114
        Bandwidth on Encoded Data / (MB/s): 3560 <= 3710 +- 190 <= 3920
        Bandwidth on Decoded Data / (MB/s): 4690 <= 4880 +- 250 <= 5160

  -> Because of missing ISA-l support, the two rapidgzip (parallel, no index) cases are much slower! Rest unchanged.

ISAL=OFF ZLIB_NG=OFF:

    Decompressed 407988639 B to 536870912 B with libarchive:
        Runtime / s: 1.373 <= 1.396 +- 0.021 <= 1.412
        Bandwidth on Encoded Data / (MB/s): 289 <= 292 +- 4 <= 297
        Bandwidth on Decoded Data / (MB/s): 380 <= 385 +- 6 <= 391
    Decompressed 407988639 B to 536870912 B with zlib:
        Runtime / s: 1.4786 <= 1.4803 +- 0.0024 <= 1.4831
        Bandwidth on Encoded Data / (MB/s): 275.1 <= 275.6 +- 0.5 <= 275.9
        Bandwidth on Decoded Data / (MB/s): 362.0 <= 362.7 +- 0.6 <= 363.1
    Decoded 15844 deflate blocks
    Decoded 15844 deflate blocks
    Decoded 15844 deflate blocks
    Decompressed 407988639 B to 536870912 B with rapidgzip (serial):
        Runtime / s: 2.149 <= 2.176 +- 0.030 <= 2.207
        Bandwidth on Encoded Data / (MB/s): 184.8 <= 187.6 +- 2.5 <= 189.9
        Bandwidth on Decoded Data / (MB/s): 243 <= 247 +- 3 <= 250
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 2.328 <= 2.347 +- 0.017 <= 2.358
        Bandwidth on Encoded Data / (MB/s): 173.0 <= 173.9 +- 1.2 <= 175.3
        Bandwidth on Decoded Data / (MB/s): 227.7 <= 228.8 +- 1.6 <= 230.7
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 0.208 <= 0.216 +- 0.011 <= 0.228
        Bandwidth on Encoded Data / (MB/s): 1790 <= 1900 +- 90 <= 1960
        Bandwidth on Decoded Data / (MB/s): 2360 <= 2490 +- 120 <= 2580
    Decompressed 407988639 B to 536870912 B with rapidgzip (parallel + index):
        Runtime / s: 0.1205 <= 0.1228 +- 0.0021 <= 0.1247
        Bandwidth on Encoded Data / (MB/s): 3270 <= 3320 +- 60 <= 3390
        Bandwidth on Decoded Data / (MB/s): 4300 <= 4370 +- 80 <= 4450

 -> Interestingly, disabling zlib-ng, also reduces libarchive performance as well as of course zlib performance.
    rapidgzip (parallel) is not much affected anymore compared to just disabling ISA-l, but encoding with an index
    gets even slower.

time gzip -d -k -c small.gz | wc -c
    real  0m2.817s
time src/tools/rapidgzip -d -k -c small.gz | wc -c
    real  0m0.343s
    user  0m1.477s
    sys   0m0.801s
time src/tools/rapidgzip -P 1 -d -k -c small.gz | wc -c
    real  0m1.236s
    user  0m0.965s
    sys   0m0.439s
  -> rapidgzip is much faster than than gzip 1.12 by now.


pigz -c small > small.pigz
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip small.pigz

    == Benchmarking with rapidgzip in parallel with different decoding chunk sizes ==

    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=0):
        Runtime / s: 0.153 <= 0.166 +- 0.015 <= 0.182
        Bandwidth on Encoded Data / (MB/s): 2240 <= 2470 +- 220 <= 2680
        Bandwidth on Decoded Data / (MB/s): 2940 <= 3250 +- 290 <= 3520
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=1):
        Runtime / s: 1.272 <= 1.278 +- 0.005 <= 1.280
        Bandwidth on Encoded Data / (MB/s): 319.0 <= 319.7 +- 1.1 <= 321.0
        Bandwidth on Decoded Data / (MB/s): 419.3 <= 420.2 +- 1.5 <= 422.0
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=2):
        Runtime / s: 0.680 <= 0.683 +- 0.004 <= 0.688
        Bandwidth on Encoded Data / (MB/s): 594 <= 598 +- 4 <= 601
        Bandwidth on Decoded Data / (MB/s): 781 <= 786 +- 5 <= 790
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=4):
        Runtime / s: 0.366 <= 0.375 +- 0.008 <= 0.381
        Bandwidth on Encoded Data / (MB/s): 1073 <= 1089 +- 23 <= 1115
        Bandwidth on Decoded Data / (MB/s): 1410 <= 1431 +- 30 <= 1465
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=8):
        Runtime / s: 0.226 <= 0.232 +- 0.005 <= 0.236
        Bandwidth on Encoded Data / (MB/s): 1730 <= 1760 +- 40 <= 1810
        Bandwidth on Decoded Data / (MB/s): 2280 <= 2320 +- 60 <= 2380
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=16):
        Runtime / s: 0.162 <= 0.180 +- 0.017 <= 0.194
        Bandwidth on Encoded Data / (MB/s): 2100 <= 2290 +- 220 <= 2530
        Bandwidth on Decoded Data / (MB/s): 2760 <= 3010 +- 290 <= 3320
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=24):
        Runtime / s: 0.146 <= 0.161 +- 0.017 <= 0.180
        Bandwidth on Encoded Data / (MB/s): 2270 <= 2560 +- 260 <= 2790
        Bandwidth on Decoded Data / (MB/s): 3000 <= 3400 +- 300 <= 3700
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=32):
        Runtime / s: 0.150 <= 0.156 +- 0.009 <= 0.166
        Bandwidth on Encoded Data / (MB/s): 2470 <= 2630 +- 140 <= 2710
        Bandwidth on Decoded Data / (MB/s): 3240 <= 3450 +- 180 <= 3570
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=64):
        Runtime / s: 0.150 <= 0.156 +- 0.005 <= 0.161
        Bandwidth on Encoded Data / (MB/s): 2540 <= 2610 +- 90 <= 2720
        Bandwidth on Decoded Data / (MB/s): 3340 <= 3430 +- 120 <= 3570
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=128):
        Runtime / s: 0.203 <= 0.212 +- 0.012 <= 0.225
        Bandwidth on Encoded Data / (MB/s): 1810 <= 1930 +- 100 <= 2010
        Bandwidth on Decoded Data / (MB/s): 2380 <= 2540 +- 140 <= 2640

  -> 64 blocks as chunks are the fastest!, starting from 24, it seems to be somewhat saturated already.
     -> Update: Now with ISA-l and zlib-ng, 24 blocks seem to be the fastest and 64 are slower in some runs.
                nBlocksToSkip=0 is near-optimal now.
     -> rapidgzip --analyze small.pigz
        - There are 37796 blocks in total
        - Each block is ~ 12800 B encoded and ~16900 B decoded,
        - Flush marker blocks (byte-aligning uncompressed blocks of size 0) appear very irregularly:
          - 25, 61, 88, 97, 196, 277, 286, 304, 342, 361, 370, 379, 399, 408, 425, 434, 475, 484, 503, 522, 531, 548,
            557, 566, 575, 584, 593, 602, 641, 660, 680, 689, 723, 732, 752, 769, 778, 787, 826, 835, 844, 864, 873, ...
            So yeah, anywhere from 9 to 99 blocks! Really might be better if I skip based on encoded data size
          - rapidgzip --analyze small.pigz | grep ': Uncompressed' | wc -l
            In total there are 2114 flush markers, so on average, after 17.88 blocks, i.e.,
            17.88 * 12800 B * 16 (flush markers to skip) = 3.5 MiB of encoded data and 4.61 MiB of decoded data.

    Decompressed 408430549 B to 536870912 B with libarchive:
        Runtime / s: 1.120 <= 1.129 +- 0.010 <= 1.140
        Bandwidth on Encoded Data / (MB/s): 358 <= 362 +- 3 <= 365
        Bandwidth on Decoded Data / (MB/s): 471 <= 476 +- 4 <= 479
    Decompressed 408430549 B to 536870912 B with zlib:
        Runtime / s: 1.199 <= 1.204 +- 0.004 <= 1.208
        Bandwidth on Encoded Data / (MB/s): 338.1 <= 339.4 +- 1.3 <= 340.6
        Bandwidth on Decoded Data / (MB/s): 444.4 <= 446.1 +- 1.7 <= 447.7
    Decoded 37796 deflate blocks
    Decoded 37796 deflate blocks
    Decoded 37796 deflate blocks
    Decompressed 408430549 B to 536870912 B with rapidgzip (serial):
        Runtime / s: 2.47 <= 2.51 +- 0.04 <= 2.55
        Bandwidth on Encoded Data / (MB/s): 160.0 <= 162.5 +- 2.6 <= 165.2
        Bandwidth on Decoded Data / (MB/s): 210 <= 214 +- 3 <= 217
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 1.229 <= 1.239 +- 0.010 <= 1.248
        Bandwidth on Encoded Data / (MB/s): 327.3 <= 329.8 +- 2.5 <= 332.4
        Bandwidth on Decoded Data / (MB/s): 430 <= 433 +- 3 <= 437
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 0.160 <= 0.161 +- 0.003 <= 0.165
        Bandwidth on Encoded Data / (MB/s): 2470 <= 2530 +- 50 <= 2560
        Bandwidth on Decoded Data / (MB/s): 3250 <= 3330 +- 60 <= 3360
    Decompressed 408430549 B to 536870912 B with rapidgzip (parallel + index):
        Runtime / s: 0.1125 <= 0.1131 +- 0.0005 <= 0.1133
        Bandwidth on Encoded Data / (MB/s): 3604 <= 3612 +- 15 <= 3629
        Bandwidth on Decoded Data / (MB/s): 4737 <= 4748 +- 19 <= 4771


bgzip -c small > small.bgz
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip small.bgz

    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=0):
        Runtime / s: 0.1276 <= 0.1291 +- 0.0014 <= 0.1303
        Bandwidth on Encoded Data / (MB/s): 3180 <= 3220 +- 40 <= 3250
        Bandwidth on Decoded Data / (MB/s): 4120 <= 4160 +- 50 <= 4210
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=1):
        Runtime / s: 1.172 <= 1.176 +- 0.003 <= 1.178
        Bandwidth on Encoded Data / (MB/s): 352.3 <= 353.1 +- 1.0 <= 354.3
        Bandwidth on Decoded Data / (MB/s): 455.7 <= 456.7 +- 1.3 <= 458.2
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=2):
        Runtime / s: 0.604 <= 0.613 +- 0.010 <= 0.623
        Bandwidth on Encoded Data / (MB/s): 666 <= 677 +- 11 <= 687
        Bandwidth on Decoded Data / (MB/s): 862 <= 876 +- 14 <= 889
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=4):
        Runtime / s: 0.329 <= 0.331 +- 0.003 <= 0.335
        Bandwidth on Encoded Data / (MB/s): 1240 <= 1252 +- 11 <= 1262
        Bandwidth on Decoded Data / (MB/s): 1603 <= 1620 +- 15 <= 1632
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=8):
        Runtime / s: 0.183 <= 0.198 +- 0.014 <= 0.211
        Bandwidth on Encoded Data / (MB/s): 1970 <= 2100 +- 150 <= 2270
        Bandwidth on Decoded Data / (MB/s): 2540 <= 2720 +- 200 <= 2930
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=16):
        Runtime / s: 0.132 <= 0.138 +- 0.005 <= 0.143
        Bandwidth on Encoded Data / (MB/s): 2910 <= 3020 +- 120 <= 3150
        Bandwidth on Decoded Data / (MB/s): 3760 <= 3910 +- 150 <= 4070
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=24):
        Runtime / s: 0.125 <= 0.128 +- 0.004 <= 0.132
        Bandwidth on Encoded Data / (MB/s): 3150 <= 3250 +- 90 <= 3320
        Bandwidth on Decoded Data / (MB/s): 4070 <= 4200 +- 120 <= 4290
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=32):
        Runtime / s: 0.132 <= 0.137 +- 0.004 <= 0.140
        Bandwidth on Encoded Data / (MB/s): 2970 <= 3030 +- 90 <= 3130
        Bandwidth on Decoded Data / (MB/s): 3850 <= 3920 +- 110 <= 4050
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=64):
        Runtime / s: 0.143 <= 0.159 +- 0.015 <= 0.173
        Bandwidth on Encoded Data / (MB/s): 2400 <= 2630 +- 250 <= 2900
        Bandwidth on Decoded Data / (MB/s): 3100 <= 3400 +- 300 <= 3700
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel, nBlocksToSkip=128):
        Runtime / s: 0.162 <= 0.169 +- 0.009 <= 0.179
        Bandwidth on Encoded Data / (MB/s): 2330 <= 2470 +- 120 <= 2560
        Bandwidth on Decoded Data / (MB/s): 3010 <= 3190 +- 160 <= 3320

  -> 32 blocks for bgz would be the best in this case!
     -> rapidgzip --analyze small.bgz
        - There are 8226 blocks and gzip streams in total
        - Each block is ~ 50400 B encoded and 65280 B (63.75 KiB) decoded,
        - So, when skipping ~32 blocks (~33 chunked), we have ~1.54 MiB encoded and ~2 MiB decoded data
          -> This is only a factor 2 less than the optimum for pigz, so that we might use the same blocking criterium,
             or not.

    Decompressed 415096389 B to 536870912 B with libarchive:
        Runtime / s: 1.208 <= 1.219 +- 0.017 <= 1.239
        Bandwidth on Encoded Data / (MB/s): 335 <= 340 +- 5 <= 344
        Bandwidth on Decoded Data / (MB/s): 433 <= 440 +- 6 <= 444
    Decompressed 415096389 B to 536870912 B with zlib:
        Runtime / s: 1.252 <= 1.268 +- 0.014 <= 1.279
        Bandwidth on Encoded Data / (MB/s): 325 <= 327 +- 4 <= 331
        Bandwidth on Decoded Data / (MB/s): 420 <= 424 +- 5 <= 429
    Decoded 8226 deflate blocks
    Decoded 8226 deflate blocks
    Decoded 8226 deflate blocks
    Decompressed 415096389 B to 536870912 B with rapidgzip (serial):
        Runtime / s: 2.825 <= 2.845 +- 0.025 <= 2.873
        Bandwidth on Encoded Data / (MB/s): 144.5 <= 145.9 +- 1.3 <= 147.0
        Bandwidth on Decoded Data / (MB/s): 186.8 <= 188.7 +- 1.7 <= 190.1
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 1.138 <= 1.156 +- 0.017 <= 1.170
        Bandwidth on Encoded Data / (MB/s): 355 <= 359 +- 5 <= 365
        Bandwidth on Decoded Data / (MB/s): 459 <= 464 +- 7 <= 472
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 0.1177 <= 0.1200 +- 0.0021 <= 0.1215
        Bandwidth on Encoded Data / (MB/s): 3420 <= 3460 +- 60 <= 3530
        Bandwidth on Decoded Data / (MB/s): 4420 <= 4470 +- 80 <= 4560
    Decompressed 415096389 B to 536870912 B with rapidgzip (parallel + index):
        Runtime / s: 0.1050 <= 0.1072 +- 0.0021 <= 0.1091
        Bandwidth on Encoded Data / (MB/s): 3800 <= 3870 +- 70 <= 3950
        Bandwidth on Decoded Data / (MB/s): 4920 <= 5010 +- 100 <= 5110

     -> ~5 GB/s for the decompressed bandwidth with the parallel bgz decoder and when decoding with an
        existing index is already quite nice!

time gzip -d -k -c small.bgz | wc -c
    real  0m3.210s
  -> Interestingly, this is reproducibly slower than the .gz compressed one. Maybe different compression setting?

time bgzip --threads $( nproc ) -d -c small.bgz | wc -c
    real  0m0.200s
  -> ~50% faster than parallel rapidgzip (~0.3s). Too much setup, memory allocations, ...?

ls -la small.*gz
    415096389 small.bgz
    416689498 small.gz
  -> The .bgz file is even smaller!


base64 /dev/urandom | head -c $(( 4*1024*1024*1024 )) > 4GiB-base64
bgzip -c 4GiB-base64 > 4GiB-base64.bgz
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip 4GiB-base64.bgz

    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=0):
        Runtime / s: 0.747 <= 0.750 +- 0.003 <= 0.754
        Bandwidth on Encoded Data / (MB/s): 4306 <= 4327 +- 19 <= 4344
        Bandwidth on Decoded Data / (MB/s): 5697 <= 5724 +- 25 <= 5747
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=1):
        Runtime / s: 7.51 <= 7.57 +- 0.06 <= 7.62
        Bandwidth on Encoded Data / (MB/s): 426 <= 429 +- 3 <= 432
        Bandwidth on Decoded Data / (MB/s): 564 <= 567 +- 4 <= 572
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=2):
        Runtime / s: 4.047 <= 4.076 +- 0.025 <= 4.093
        Bandwidth on Encoded Data / (MB/s): 793 <= 797 +- 5 <= 802
        Bandwidth on Decoded Data / (MB/s): 1049 <= 1054 +- 7 <= 1061
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=4):
        Runtime / s: 2.090 <= 2.093 +- 0.005 <= 2.099
        Bandwidth on Encoded Data / (MB/s): 1547 <= 1551 +- 4 <= 1553
        Bandwidth on Decoded Data / (MB/s): 2046 <= 2052 +- 5 <= 2055
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=8):
        Runtime / s: 1.115 <= 1.125 +- 0.010 <= 1.136
        Bandwidth on Encoded Data / (MB/s): 2859 <= 2886 +- 26 <= 2911
        Bandwidth on Decoded Data / (MB/s): 3780 <= 3820 +- 30 <= 3850
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=16):
        Runtime / s: 0.735 <= 0.764 +- 0.030 <= 0.795
        Bandwidth on Encoded Data / (MB/s): 4090 <= 4260 +- 170 <= 4420
        Bandwidth on Decoded Data / (MB/s): 5410 <= 5630 +- 220 <= 5840
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=24):
        Runtime / s: 0.759 <= 0.770 +- 0.015 <= 0.788
        Bandwidth on Encoded Data / (MB/s): 4120 <= 4210 +- 80 <= 4280
        Bandwidth on Decoded Data / (MB/s): 5450 <= 5580 +- 110 <= 5660
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=32):
        Runtime / s: 0.820 <= 0.831 +- 0.011 <= 0.841
        Bandwidth on Encoded Data / (MB/s): 3860 <= 3910 +- 50 <= 3960
        Bandwidth on Decoded Data / (MB/s): 5110 <= 5170 +- 70 <= 5240
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=64):
        Runtime / s: 0.909 <= 0.921 +- 0.011 <= 0.931
        Bandwidth on Encoded Data / (MB/s): 3490 <= 3520 +- 40 <= 3570
        Bandwidth on Decoded Data / (MB/s): 4610 <= 4660 +- 60 <= 4730
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=128):
        Runtime / s: 1.002 <= 1.028 +- 0.022 <= 1.042
        Bandwidth on Encoded Data / (MB/s): 3120 <= 3160 +- 70 <= 3240
        Bandwidth on Decoded Data / (MB/s): 4120 <= 4180 +- 90 <= 4290

  -> The peak is at 64, close to the last.
     Update 2026-11: Now the peak is pretty clearly at 16 and nBlocksToSkip=0 is pretty optimal
     -> So we are fine using a chunking criterium shared with pigz of roughly 4 MiB of encoded data or maybe more.
        -> Note that for generic gzip files, we would need roughly 32 MiB of encoded data because of the much slower
           block finder amortizing slower!

    Decompressed 3246516414 B to 4294967296 B with libarchive:
        Runtime / s: 8.38 <= 8.57 +- 0.17 <= 8.70
        Bandwidth on Encoded Data / (MB/s): 373 <= 379 +- 8 <= 388
        Bandwidth on Decoded Data / (MB/s): 494 <= 501 +- 10 <= 513
    Decompressed 3246516414 B to 4294967296 B with zlib:
        Runtime / s: 9.225 <= 9.237 +- 0.020 <= 9.260
        Bandwidth on Encoded Data / (MB/s): 350.6 <= 351.5 +- 0.8 <= 351.9
        Bandwidth on Decoded Data / (MB/s): 463.8 <= 465.0 +- 1.0 <= 465.6
    Decoded 65857 deflate blocks
    Decoded 65857 deflate blocks
    Decoded 65857 deflate blocks
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (serial):
        Runtime / s: 16.63 <= 16.70 +- 0.08 <= 16.78
        Bandwidth on Encoded Data / (MB/s): 193.5 <= 194.4 +- 0.9 <= 195.3
        Bandwidth on Decoded Data / (MB/s): 256.0 <= 257.2 +- 1.2 <= 258.3
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 7.52 <= 7.60 +- 0.07 <= 7.65
        Bandwidth on Encoded Data / (MB/s): 424 <= 427 +- 4 <= 432
        Bandwidth on Decoded Data / (MB/s): 561 <= 565 +- 5 <= 571
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 0.732 <= 0.745 +- 0.011 <= 0.753
        Bandwidth on Encoded Data / (MB/s): 4310 <= 4360 +- 70 <= 4430
        Bandwidth on Decoded Data / (MB/s): 5710 <= 5770 +- 90 <= 5870
    Decompressed 3246516414 B to 4294967296 B with rapidgzip (parallel + index):
        Runtime / s: 0.76 <= 0.78 +- 0.03 <= 0.82
        Bandwidth on Encoded Data / (MB/s): 3970 <= 4160 +- 170 <= 4270
        Bandwidth on Decoded Data / (MB/s): 5250 <= 5500 +- 220 <= 5650

      -> Why is the version with an index actually slower!?
         I almost would assume that zlib is less optimized than the rapidgzip-internal inflate for gzip header/footer
         reading and therefore is suboptimal for this one gzip stream per deflate block file format (BGZF).
         -> Has been circumvented now that ISA-L is used

time src/tools/rapidgzip -d -c 4GiB-base64.bgz | wc -c
    real  0m2.064s
  -> The CLI has much larger overhead than the pure benchmarks in this suite!
     More than 2x slower! Memory allocations?
time bgzip --threads $( nproc ) -d -c 4GiB-base64.bgz | wc -c
    real  0m1.628s
time bgzip --threads $( nproc ) -d -c 4GiB-base64.bgz > /dev/null
    real  0m0.783s

time rapidgzip -d -c 4GiB-base64.bgz | wc -c
    real  0m2.019s
  -> This was faster in the past with 1.3s! Not sure what went wrong. The splicing bugs?
time rapidgzip -d -c 4GiB-base64.bgz > /dev/null
    real  0m0.581s
  -> 30% faster than bgzip thanks to internal usage of ISA-L


base64 /dev/urandom | head -c $(( 1024*1024*1024 )) | pigz > 4GiB-base64.pigz
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip 4GiB-base64.pigz

    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=0):
        Runtime / s: 1.032 <= 1.036 +- 0.005 <= 1.042
        Bandwidth on Encoded Data / (MB/s): 3136 <= 3154 +- 16 <= 3166
        Bandwidth on Decoded Data / (MB/s): 4122 <= 4146 +- 21 <= 4161
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=1):
        Runtime / s: 9.70 <= 9.80 +- 0.13 <= 9.94
        Bandwidth on Encoded Data / (MB/s): 329 <= 334 +- 4 <= 337
        Bandwidth on Decoded Data / (MB/s): 432 <= 439 +- 6 <= 443
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=2):
        Runtime / s: 5.26 <= 5.31 +- 0.05 <= 5.37
        Bandwidth on Encoded Data / (MB/s): 609 <= 615 +- 6 <= 621
        Bandwidth on Decoded Data / (MB/s): 800 <= 808 +- 8 <= 816
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=4):
        Runtime / s: 2.765 <= 2.777 +- 0.013 <= 2.790
        Bandwidth on Encoded Data / (MB/s): 1171 <= 1177 +- 5 <= 1182
        Bandwidth on Decoded Data / (MB/s): 1540 <= 1547 +- 7 <= 1554
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=8):
        Runtime / s: 1.47 <= 1.51 +- 0.03 <= 1.53
        Bandwidth on Encoded Data / (MB/s): 2140 <= 2170 +- 50 <= 2220
        Bandwidth on Decoded Data / (MB/s): 2810 <= 2850 +- 60 <= 2920
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=16):
        Runtime / s: 1.161 <= 1.183 +- 0.019 <= 1.197
        Bandwidth on Encoded Data / (MB/s): 2730 <= 2760 +- 50 <= 2810
        Bandwidth on Decoded Data / (MB/s): 3590 <= 3630 +- 60 <= 3700
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=24):
        Runtime / s: 1.037 <= 1.046 +- 0.014 <= 1.062
        Bandwidth on Encoded Data / (MB/s): 3080 <= 3120 +- 40 <= 3150
        Bandwidth on Decoded Data / (MB/s): 4050 <= 4110 +- 50 <= 4140
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=32):
        Runtime / s: 1.012 <= 1.031 +- 0.018 <= 1.047
        Bandwidth on Encoded Data / (MB/s): 3120 <= 3170 +- 60 <= 3230
        Bandwidth on Decoded Data / (MB/s): 4100 <= 4170 +- 70 <= 4240
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=64):
        Runtime / s: 1.034 <= 1.048 +- 0.013 <= 1.060
        Bandwidth on Encoded Data / (MB/s): 3080 <= 3120 +- 40 <= 3160
        Bandwidth on Decoded Data / (MB/s): 4050 <= 4100 +- 50 <= 4150
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel, nBlocksToSkip=128):
        Runtime / s: 1.144 <= 1.151 +- 0.009 <= 1.162
        Bandwidth on Encoded Data / (MB/s): 2813 <= 2838 +- 22 <= 2856
        Bandwidth on Decoded Data / (MB/s): 3698 <= 3731 +- 30 <= 3754

    Decompressed 3267440860 B to 4294967296 B with libarchive:
        Runtime / s: 9.12 <= 9.18 +- 0.11 <= 9.32
        Bandwidth on Encoded Data / (MB/s): 351 <= 356 +- 4 <= 358
        Bandwidth on Decoded Data / (MB/s): 461 <= 468 +- 6 <= 471
    Decompressed 3267440860 B to 4294967296 B with zlib:
        Runtime / s: 9.48 <= 9.63 +- 0.18 <= 9.82
        Bandwidth on Encoded Data / (MB/s): 333 <= 339 +- 6 <= 345
        Bandwidth on Decoded Data / (MB/s): 437 <= 446 +- 8 <= 453
    Decoded 303227 deflate blocks
    Decoded 303227 deflate blocks
    Decoded 303227 deflate blocks
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (serial):
        Runtime / s: 19.82 <= 19.98 +- 0.15 <= 20.12
        Bandwidth on Encoded Data / (MB/s): 162.4 <= 163.6 +- 1.2 <= 164.8
        Bandwidth on Decoded Data / (MB/s): 213.5 <= 215.0 +- 1.6 <= 216.7
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 9.80 <= 9.93 +- 0.13 <= 10.07
        Bandwidth on Encoded Data / (MB/s): 325 <= 329 +- 4 <= 333
        Bandwidth on Decoded Data / (MB/s): 427 <= 432 +- 6 <= 438
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 1.13 <= 1.16 +- 0.04 <= 1.20
        Bandwidth on Encoded Data / (MB/s): 2710 <= 2820 +- 90 <= 2880
        Bandwidth on Decoded Data / (MB/s): 3570 <= 3710 +- 120 <= 3790
    Decompressed 3267440860 B to 4294967296 B with rapidgzip (parallel + index):
        Runtime / s: 0.904 <= 0.912 +- 0.013 <= 0.927
        Bandwidth on Encoded Data / (MB/s): 3520 <= 3580 +- 50 <= 3620
        Bandwidth on Decoded Data / (MB/s): 4630 <= 4710 +- 70 <= 4750

  -> Rapidgzip has abysmal serial decoder speed, probably because pigz deflate blocks are only 16 KiB so that the
     upfront cost for the double-symbol cached Huffman-decoder becomes expensive.
     With ISA-l (parallel: 1 core), it is better, though.


# Benchmark with DECOMPRESSION_BUFFER_SIZE = 8_Mi:

## With ISA-L

zlib should not matter for rapidgzip if ISA-L is available. It matters for the libarchive benchmark though!

for (( i=0; i<20; ++i )); do cat test-files/silesia/silesia.tar; done | gzip > silesia-20x.tar.gz
cmake -DLIBRAPIDARCHIVE_WITH_ISAL=ON \
      -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=ON \
      -DLIBRAPIDARCHIVE_USE_ZLIB_NG=OFF .. &&
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip silesia-20x.tar.gz

    Decompressed 1364784177 B to 4239155200 B with libarchive:
        Runtime / s: 10.19 <= 10.23 +- 0.04 <= 10.26
        Bandwidth on Encoded Data / (MB/s): 133.0 <= 133.4 +- 0.5 <= 134.0
        Bandwidth on Decoded Data / (MB/s): 413.2 <= 414.3 +- 1.7 <= 416.2
    Decompressed 1364784177 B to 4239155200 B with zlib:
        Runtime / s: 10.31 <= 10.46 +- 0.15 <= 10.61
        Bandwidth on Encoded Data / (MB/s): 128.7 <= 130.5 +- 1.8 <= 132.4
        Bandwidth on Decoded Data / (MB/s): 400 <= 405 +- 6 <= 411
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (serial):
        Runtime / s: 10.08 <= 10.21 +- 0.13 <= 10.33
        Bandwidth on Encoded Data / (MB/s): 132.1 <= 133.7 +- 1.6 <= 135.4
        Bandwidth on Decoded Data / (MB/s): 410 <= 415 +- 5 <= 420
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 6.80 <= 6.84 +- 0.06 <= 6.91
        Bandwidth on Encoded Data / (MB/s): 197.6 <= 199.7 +- 1.8 <= 200.7
        Bandwidth on Decoded Data / (MB/s): 614 <= 620 +- 5 <= 623
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 1.415 <= 1.426 +- 0.016 <= 1.445
        Bandwidth on Encoded Data / (MB/s): 945 <= 957 +- 11 <= 965
        Bandwidth on Decoded Data / (MB/s): 2930 <= 2970 +- 30 <= 3000
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel + index):
        Runtime / s: 0.794 <= 0.810 +- 0.021 <= 0.833
        Bandwidth on Encoded Data / (MB/s): 1640 <= 1680 +- 40 <= 1720
        Bandwidth on Decoded Data / (MB/s): 5090 <= 5230 +- 130 <= 5340

  -> rapidgzip (parallel: 1 cores) is faster than rapidgzip (serial) because GzipReader does not fall back to ISA-L!

## Without ISA-L (System Zlib)

cmake -DLIBRAPIDARCHIVE_WITH_ISAL=OFF \
      -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=ON \
      -DLIBRAPIDARCHIVE_USE_ZLIB_NG=OFF .. &&
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip silesia-20x.tar.gz

    Decompressed 1364784177 B to 4239155200 B with libarchive:
        Runtime / s: 9.99 <= 10.05 +- 0.08 <= 10.13
        Bandwidth on Encoded Data / (MB/s): 134.7 <= 135.9 +- 1.0 <= 136.6
        Bandwidth on Decoded Data / (MB/s): 418 <= 422 +- 3 <= 424
    Decompressed 1364784177 B to 4239155200 B with zlib:
        Runtime / s: 10.38 <= 10.45 +- 0.12 <= 10.58
        Bandwidth on Encoded Data / (MB/s): 128.9 <= 130.6 +- 1.5 <= 131.5
        Bandwidth on Decoded Data / (MB/s): 400 <= 406 +- 5 <= 409
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (serial):
        Runtime / s: 10.90 <= 10.96 +- 0.07 <= 11.04
        Bandwidth on Encoded Data / (MB/s): 123.6 <= 124.5 +- 0.8 <= 125.2
        Bandwidth on Decoded Data / (MB/s): 384.0 <= 386.6 +- 2.5 <= 388.9
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 12.383 <= 12.408 +- 0.025 <= 12.434
        Bandwidth on Encoded Data / (MB/s): 109.76 <= 109.99 +- 0.23 <= 110.21
        Bandwidth on Decoded Data / (MB/s): 340.9 <= 341.6 +- 0.7 <= 342.3
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 1.58 <= 1.62 +- 0.04 <= 1.66
        Bandwidth on Encoded Data / (MB/s): 823 <= 844 +- 20 <= 864
        Bandwidth on Decoded Data / (MB/s): 2560 <= 2620 +- 60 <= 2680
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel + index):
        Runtime / s: 0.878 <= 0.888 +- 0.009 <= 0.895
        Bandwidth on Encoded Data / (MB/s): 1525 <= 1537 +- 16 <= 1554
        Bandwidth on Decoded Data / (MB/s): 4740 <= 4770 +- 50 <= 4830

  -> rapidgzip (parallel: 1 cores) is 10% slower than rapidgzip (serial). This is because it does not use zlib
     but our custom-written decompressor! ZlibInflateWrapper is missing the StoppingPoint API, that's why. See below.

## Without ISA-L (Static zlib)

cmake -DLIBRAPIDARCHIVE_WITH_ISAL=OFF \
      -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=OFF \
      -DLIBRAPIDARCHIVE_USE_ZLIB_NG=OFF .. &&
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip silesia-20x.tar.gz

    Decompressed 1364784177 B to 4239155200 B with libarchive:
        Runtime / s: 10.669 <= 10.688 +- 0.029 <= 10.722
        Bandwidth on Encoded Data / (MB/s): 127.3 <= 127.7 +- 0.3 <= 127.9
        Bandwidth on Decoded Data / (MB/s): 395.4 <= 396.6 +- 1.1 <= 397.4
    Decompressed 1364784177 B to 4239155200 B with zlib:
        Runtime / s: 10.872 <= 10.890 +- 0.028 <= 10.922
        Bandwidth on Encoded Data / (MB/s): 125.0 <= 125.3 +- 0.3 <= 125.5
        Bandwidth on Decoded Data / (MB/s): 388.1 <= 389.3 +- 1.0 <= 389.9
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (serial):
        Runtime / s: 11.09 <= 11.16 +- 0.07 <= 11.22
        Bandwidth on Encoded Data / (MB/s): 121.6 <= 122.3 +- 0.7 <= 123.1
        Bandwidth on Decoded Data / (MB/s): 377.8 <= 379.9 +- 2.3 <= 382.3
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 12.67 <= 12.72 +- 0.08 <= 12.80
        Bandwidth on Encoded Data / (MB/s): 106.6 <= 107.3 +- 0.6 <= 107.7
        Bandwidth on Decoded Data / (MB/s): 331.1 <= 333.4 +- 2.0 <= 334.7
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 1.54 <= 1.58 +- 0.04 <= 1.61
        Bandwidth on Encoded Data / (MB/s): 846 <= 866 +- 19 <= 885
        Bandwidth on Decoded Data / (MB/s): 2630 <= 2690 +- 60 <= 2750
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel + index):
        Runtime / s: 0.905 <= 0.911 +- 0.009 <= 0.921
        Bandwidth on Encoded Data / (MB/s): 1482 <= 1499 +- 14 <= 1507
        Bandwidth on Decoded Data / (MB/s): 4600 <= 4660 +- 40 <= 4680

 -> Not much different from system zlib


## Without ISA-L (Static zlib-ng)

cmake -DLIBRAPIDARCHIVE_WITH_ISAL=OFF \
      -DLIBRAPIDARCHIVE_USE_SYSTEM_ZLIB=OFF \
      -DLIBRAPIDARCHIVE_USE_ZLIB_NG=ON .. &&
cmake --build . -- benchmarkGzip && src/benchmarks/benchmarkGzip silesia-20x.tar.gz

    Decompressed 1364784177 B to 4239155200 B with libarchive:
        Runtime / s: 5.673 <= 5.699 +- 0.024 <= 5.722
        Bandwidth on Encoded Data / (MB/s): 238.5 <= 239.5 +- 1.0 <= 240.6
        Bandwidth on Decoded Data / (MB/s): 741 <= 744 +- 3 <= 747
    Decompressed 1364784177 B to 4239155200 B with zlib:
        Runtime / s: 5.99 <= 6.03 +- 0.06 <= 6.09
        Bandwidth on Encoded Data / (MB/s): 224.0 <= 226.3 +- 2.1 <= 228.0
        Bandwidth on Decoded Data / (MB/s): 696 <= 703 +- 7 <= 708
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decoded 65997 deflate blocks
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (serial):
        Runtime / s: 11.09 <= 11.16 +- 0.10 <= 11.28
        Bandwidth on Encoded Data / (MB/s): 121.0 <= 122.2 +- 1.1 <= 123.1
        Bandwidth on Decoded Data / (MB/s): 376 <= 380 +- 3 <= 382
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 12.37 <= 12.47 +- 0.12 <= 12.61
        Bandwidth on Encoded Data / (MB/s): 108.2 <= 109.4 +- 1.1 <= 110.3
        Bandwidth on Decoded Data / (MB/s): 336 <= 340 +- 3 <= 343
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 1.548 <= 1.577 +- 0.027 <= 1.601
        Bandwidth on Encoded Data / (MB/s): 852 <= 866 +- 15 <= 882
        Bandwidth on Decoded Data / (MB/s): 2650 <= 2690 +- 50 <= 2740
    Decompressed 1364784177 B to 4239155200 B with rapidgzip (parallel + index):
        Runtime / s: 0.720 <= 0.724 +- 0.004 <= 0.728
        Bandwidth on Encoded Data / (MB/s): 1874 <= 1885 +- 11 <= 1895
        Bandwidth on Decoded Data / (MB/s): 5820 <= 5850 +- 30 <= 5890

 -> The zlib and even the libarchive decompressions are 50% faster!
 -> The decompression with index is actually faster ~5% faster than ISA-L!
 -> "rapidgzip (parallel: 1 cores)" does not profit at all because the until the untilOffset not being exactly
    known makes it not use decodeChunkWithInflateWrapper. finishDecodeChunkWithInexactOffset is used because
    the window is known, but it does not work with ZlibInflateWrapper because the StoppingPoint API is not implemented.
    src/tools/rapidgzip --count -v silesia-20x.tar.gz
        Time spent decoding with custom inflate  : 18.4073 s
        Time spent decoding with inflate wrapper : 0 s
        Time spent decoding with ISA-L           : 0 s
 -> Parallel decompression without index is ~5 % slower than ISA-L but ~10 % faster than zlib! Very nice!

## Without ISA-L (Static zlib-ng and with name mangling)

In CMakeLists.txt added set(ZLIB_SYMBOL_PREFIX LIBRAPIDARCHIVE_) before configuring.

    Decompressed 1364776140 B to 4239155200 B with libarchive:
        Runtime / s: 10.14 <= 10.21 +- 0.09 <= 10.32
        Bandwidth on Encoded Data / (MB/s): 132.3 <= 133.7 +- 1.2 <= 134.6
        Bandwidth on Decoded Data / (MB/s): 411 <= 415 +- 4 <= 418
    Decompressed 1364776140 B to 4239155200 B with zlib:
        Runtime / s: 7.08 <= 7.17 +- 0.08 <= 7.22
        Bandwidth on Encoded Data / (MB/s): 189.0 <= 190.4 +- 2.1 <= 192.8
        Bandwidth on Decoded Data / (MB/s): 587 <= 591 +- 6 <= 599
    Decoded 66040 deflate blocks
    Decoded 66040 deflate blocks
    Decoded 66040 deflate blocks
    Decompressed 1364776140 B to 4239155200 B with rapidgzip (serial):
        Runtime / s: 12.20 <= 12.45 +- 0.29 <= 12.77
        Bandwidth on Encoded Data / (MB/s): 106.9 <= 109.7 +- 2.5 <= 111.8
        Bandwidth on Decoded Data / (MB/s): 332 <= 341 +- 8 <= 347
    Decompressed 1364776140 B to 4239155200 B with rapidgzip (parallel: 1 cores):
        Runtime / s: 13.38 <= 13.50 +- 0.15 <= 13.67
        Bandwidth on Encoded Data / (MB/s): 99.8 <= 101.1 +- 1.1 <= 102.0
        Bandwidth on Decoded Data / (MB/s): 310 <= 314 +- 3 <= 317
    Decompressed 1364776140 B to 4239155200 B with rapidgzip (parallel: 24 cores):
        Runtime / s: 1.70 <= 1.73 +- 0.03 <= 1.76
        Bandwidth on Encoded Data / (MB/s): 777 <= 789 +- 14 <= 804
        Bandwidth on Decoded Data / (MB/s): 2410 <= 2450 +- 40 <= 2500
    Decompressed 1364776140 B to 4239155200 B with rapidgzip (parallel + index):
        Runtime / s: 0.883 <= 0.888 +- 0.005 <= 0.893
        Bandwidth on Encoded Data / (MB/s): 1528 <= 1538 +- 9 <= 1545
        Bandwidth on Decoded Data / (MB/s): 4745 <= 4777 +- 28 <= 4800

 -> Libarchive, which is used as a shared library, now keeps using the system ZLIB, instead of effectively
    being preloaded by our own statically built zlib-version thanks to the ZLIB_SYMBOL_PREFIX!
    I think this should be much safer, especially for the CPython shared library!
    Else, we could be accidentally replacing zlib calls, which may or may not lead to errors!
 -> Everything else uses the statically built zlib-ng. For good measure, I also set Z_PREFIX to ZLIB_SYMBOL_PREFIX
    in case the included zlib.h is the system one instead of the zlib-ng one. This also avoids problems with
    include precedence! This results in our usages of zlib, such as 'inflate' actually being defines that replaced
    with 'LIBRAPARCHIVE_inflate' and so on.
*/

/**
sudo apt install gzip tabix pigz libarchive-dev zlib1g-dev isal
python3 -m pip install --user pgzip indexed_gzip

base64 /dev/urandom | head -c $(( 512*1024*1024 )) > small
gzip -k small
tar -cf small.tar small
gzip small.tar

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.open("small.gz").read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 4.594 s

python3 -c '
import indexed_gzip as igz;
import time; t0 = time.time();
with igz.open("10x.silesia.tar.gz") as file:
    while result := file.read( 16*1024*1024 ):
        pass
print(f"Decompression took {time.time() - t0:.3f} s")
'

    Chunk size | decompression time
    -----------+-------------------
         4 MiB | 14.3 s
        16 MiB | 11.3 s
        64 MiB | 11.3 s
       256 MiB | 10.9 s
      1024 MiB | 11.5 s
           inf | 13.1 s

 -> Note small.gz as used in the other benchmarks is only 398 MiB compressed, so that the "inf" case,
    which is used for the other benchmarks, should be almost optimal.

python3 -c 'import gzip; import time; t0 = time.time(); gzip.open("small.gz").read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 3.069 s

time python3 -m pgzip -o - small.gz | wc -c
    416755811

    real	0m3.289s
    user	0m10.282s
    sys	0m0.633s

time archivemount small.tar.gz mountpoint/
    real	0m1.657s
    user	0m1.563s
    sys	0m0.070s

time ( fuse-archive small.tar.gz mountpoint && stat mountpoint &>/dev/null; )
    real	0m1.697s
    user	0m0.002s
    sys	0m0.015s

time gzip -d -k -c small.gz | wc -c
    536870912

    real	0m3.510s
    user	0m3.395s
    sys	0m0.283s

time pigz -d -k -c small.gz | wc -c
    536870912

    real	0m2.225s
    user	0m2.797s
    sys	0m0.524s

time bgzip -d -c small.gz | wc -c
    536870912

    real	0m2.548s
    user	0m2.411s
    sys	0m0.253s

time igzip -d -c small.gz | wc -c
    536870912

    real	0m1.523s
    user	0m1.307s
    sys	0m0.344s

time pugz small.gz | wc -c
    536870912

    real	0m2.372s
    user	0m2.273s
    sys	0m0.262s

time pugz -t 4 small.gz | wc -c
    using 4 threads for decompression (experimental)
    536870912

    real	0m0.975s
    user	0m2.345s
    sys	0m0.318s

time pugz -t $( nproc ) small.gz | wc -c
    using 24 threads for decompression (experimental)
    536870912

    real	0m0.985s
    user	0m2.828s
    sys	0m6.894s

cd zlib-ng/ && mkdir build && cd $_ && cmake .. && cmake --build . --config Release
cp minigzip ~/bin/minigzip-zlib-ng
time minigzip-zlib-ng -d -k -c small.gz | wc -c
    536870912

    real	0m1.903s
    user	0m1.743s
    sys	0m0.283s

cd libdeflate && make && cp gunzio ~/bin/gunzip-libdeflate
time gunzip-libdeflate -d -k -c small.gz | wc -c
    536870912

    real	0m1.841s
    user	0m1.508s
    sys	0m0.460s

time crc32 small
    474e5ffd

    real	0m0.510s
    user	0m0.421s
    sys	0m0.069s
*/


/*
Repeat benchmarks with tarred and gzipped Silesia corpus.
http://sun.aei.polsl.pl/~sdeor/index.php?page=silesia

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.open("small.gz").read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 1.249 s

python3 -c 'import gzip; import time; t0 = time.time(); gzip.open("small.gz").read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 0.908 s

time python3 -m pgzip -o - small.gz | wc -c
    67997404

    real	0m1.837s
    user	0m1.719s
    sys	0m0.128s

time archivemount small.tar.gz mountpoint/
    real	0m1.663s
    user	0m1.580s
    sys	0m0.053s

time ( fuse-archive small.tar.gz mountpoint && stat mountpoint &>/dev/null; )
    real	0m1.629s
    user	0m0.007s
    sys	0m0.008s

time gzip -d -k -c small.gz | wc -c
    211957760

    real	0m1.000s
    user	0m0.958s
    sys	0m0.102s

time pigz -d -k -c small.gz | wc -c
    211957760

    real	0m0.622s
    user	0m0.860s
    sys	0m0.176s

time bgzip -d -c small.gz | wc -c
    211957760

    real	0m0.700s
    user	0m0.667s
    sys	0m0.068s

time igzip -d -c small.gz | wc -c
    211957760

    real	0m0.357s
    user	0m0.299s
    sys	0m0.109s

time pugz small.gz | wc -c
    terminate called after throwing an instance of 'gzip_error'
      what():  INVALID_LITERAL
    0

    real	0m0.078s
    user	0m0.005s
    sys	0m0.006s

time minigzip-zlib-ng -d -k -c small.gz | wc -c
    211957760

    real	0m0.443s
    user	0m0.405s
    sys	0m0.081s

time gunzip-libdeflate -d -k -c small.gz | wc -c
    211957760

    real	0m0.403s
    user	0m0.276s
    sys	0m0.184s

time crc32 silesia.tar
    78e42bf0

    real	0m0.236s
    user	0m0.174s
    sys	0m0.043s


Rebenchmark different versions and options of indexed_gzip

python3 -m pip install --user pgzip indexed_gzip
python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.open("small.gz").read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 4.666 s

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.IndexedGzipFile("small.gz", spacing=16*1024**2).read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 6.403 s

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.IndexedGzipFile("small.gz", spacing=2**30, readbuf_size=2**30, buffer_size=2**30).read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Floating point exception

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.open("small.gz", spacing=int(1*1024**2), readbuf_size=int(1*1024**2), buffer_size=int(1*1024**2)).read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 4.286 s

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.open("small.gz", spacing=int(32*1024**2), readbuf_size=int(1*1024**2), buffer_size=int(1*1024**2)).read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 7.407 s

python3 -c 'import indexed_gzip as igz; import time; t0 = time.time(); igz.open("small.gz", spacing=int(128*1024**2), readbuf_size=int(1024**2), buffer_size=int(128*1024**2)).read(); print(f"Decompression took {time.time() - t0:.3f} s")'
    Decompression took 13.290 s
*/


/*
git clone https://github.com/mxmlnkn/librapidarchive
cd librapidarchive && mkdir build && cd build && cmake ..
sudo apt install pigz isal tabix

# Create a compressible random file
fname=4GiB-base64
base64 /dev/urandom | head -c $(( 4 * 1024 * 1024 * 1024 )) > "$fname"
pigz  -k -c "$fname" > "$fname.pigz"
bgzip    -c "$fname" > "$fname.bgz"
gzip  -k -c "$fname" > "$fname.gz"
igzip -k -c "$fname" > "$fname.igz"

printf '\n| %6s | %19s | %10s | %18s |\n' Format Decoder 'Runtime / s' 'Bandwidth / (MB/s)'
printf -- '|--------|---------------------|-------------|--------------------|\n'
for tool in cat fcat; do
    runtime=$( ( time $tool "$fname" | wc -c ) 2>&1 | sed -n -E 's|real[ \t]*0m||p' | sed 's|[ \ts]||' )
    bandwidth=$( python3 -c "print( int( round( $fileSize / 1e6 / $runtime ) ) )" )
    printf '| %6s | %19s | %11s | %18s |\n' "" "$tool" "$runtime" "$bandwidth"
done

fname=4GiB-base64
fileSize=$( stat -L --format=%s "$fname" )
for format in gz bgz pigz igz; do
    printf '\n| %6s | %15s | %10s | %18s |\n' Format Decoder 'Runtime / s' 'Bandwidth / (MB/s)'
    printf -- '|--------|-----------------|-------------|--------------------|\n'
    crc32 "$fname.$format" &>/dev/null  # Make my QLC SSD cache the file into the SLC cache
    crc32 "$fname.$format" &>/dev/null  # Make my QLC SSD cache the file into the SLC cache

    for tool in gzip bgzip "bgzip -@ $( nproc )" pigz igzip "igzip -T $( nproc )" "src/tools/rapidgzip -P 1" "src/tools/rapidgzip -P $( nproc )"; do
        runtime=$( ( time $tool -d -c "$fname.$format" | wc -c ) 2>&1 | sed -n -E 's|real[ \t]*0m||p' | sed 's|[ \ts]||' )
        bandwidth=$( python3 -c "print( int( round( $fileSize / 1e6 / $runtime ) ) )" )
        printf '| %6s | %15s | %11s | %18s |\n' "$format" "${tool#src/tools/}" "$runtime" "$bandwidth"
    done
done

    | Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
    |--------|---------------|-------------|--------------------|
    |     gz |          gzip |      22.218 |                193 |
    |     gz |         bgzip |      16.202 |                265 |
    |     gz |   bgzip -@ 24 |      15.962 |                269 |
    |     gz |          pigz |      13.391 |                321 |
    |     gz |         igzip |       9.225 |                466 |
    |     gz |   igzip -T 24 |       9.295 |                462 |
    |     gz | rapidgzip -P 1 |       8.811 |                487 |
    |     gz | rapidgzip -P 24 |       1.320 |               **3254** |

    | Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
    |--------|---------------|-------------|--------------------|
    |    bgz |          gzip |      21.346 |                201 |
    |    bgz |         bgzip |      10.621 |                404 |
    |    bgz |   bgzip -@ 24 |       1.949 |               2204 |
    |    bgz |          pigz |      18.466 |                233 |
    |    bgz |         igzip |       7.321 |                587 |
    |    bgz |   igzip -T 24 |       7.377 |                582 |
    |    bgz | rapidgzip -P 1 |       7.520 |                571 |
    |    bgz | rapidgzip -P 24 |       1.125 |               **3818** |

    | Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
    |--------|---------------|-------------|--------------------|
    |   pigz |          gzip |      24.320 |                177 |
    |   pigz |         bgzip |      16.358 |                263 |
    |   pigz |   bgzip -@ 24 |      15.868 |                271 |
    |   pigz |          pigz |      13.404 |                320 |
    |   pigz |         igzip |       9.709 |                442 |
    |   pigz |   igzip -T 24 |       9.491 |                453 |
    |   pigz | rapidgzip -P 1 |       8.867 |                484 |
    |   pigz | rapidgzip -P 24 |       1.288 |               **3335** |

    | Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
    |--------|---------------|-------------|--------------------|
    |    igz |          gzip |      22.809 |                188 |
    |    igz |         bgzip |      15.302 |                281 |
    |    igz |   bgzip -@ 24 |      15.170 |                283 |
    |    igz |          pigz |      12.416 |                346 |
    |    igz |         igzip |       7.836 |                548 |
    |    igz |   igzip -T 24 |       7.797 |                551 |
    |    igz | rapidgzip -P 1 |       7.149 |                601 |
    |    igz | rapidgzip -P 24 |       1.293 |               **3322** |


# Create an incompressible random file
fileSize=$(( 4 * 1024 * 1024 * 1024 ))
fname=4GiB-random
head -c $fileSize /dev/urandom > $fname
pigz  -k -c "$fname" > "$fname.pigz"
bgzip    -c "$fname" > "$fname.bgz"
gzip  -k -c "$fname" > "$fname.gz"
igzip -k -c "$fname" > "$fname.igz"

printf '\n| %6s | %19s | %10s | %18s |\n' Format Decoder 'Runtime / s' 'Bandwidth / (MB/s)'
printf -- '|--------|---------------------|-------------|--------------------|\n'
for tool in cat fcat; do
    runtime=$( ( time $tool "$fname" | wc -c ) 2>&1 | sed -n -E 's|real[ \t]*0m||p' | sed 's|[ \ts]||' )
    bandwidth=$( python3 -c "print( int( round( $fileSize / 1e6 / $runtime ) ) )" )
    printf '| %6s | %19s | %11s | %18s |\n' "" "$tool" "$runtime" "$bandwidth"
done

fname=4GiB-random
fileSize=$( stat -L --format=%s -- "$fname" )

for format in gz bgz pigz igz; do
    printf '\n| %6s | %19s | %10s | %18s |\n' Format Decoder 'Runtime / s' 'Bandwidth / (MB/s)'
    printf -- '|--------|---------------------|-------------|--------------------|\n'
    crc32 "$fname.$format" &>/dev/null  # Make my QLC SSD cache the file into the SLC cache

    for tool in gzip bgzip "bgzip -@ $( nproc )" pigz igzip "igzip -T $( nproc )" "src/tools/rapidgzip -P 1" "src/tools/rapidgzip -P $( nproc )"; do
        runtime=$( ( time $tool -d -c "$fname.$format" | wc -c ) 2>&1 | sed -n -E 's|real[ \t]*0m||p' | sed 's|[ \ts]||' )
        bandwidth=$( python3 -c "print( int( round( $fileSize / 1e6 / $runtime ) ) )" )
        printf '| %6s | %19s | %11s | %18s |\n' "$format" "$tool" "$runtime" "$bandwidth"
    done
done

| Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
|--------|---------------|-------------|--------------------|
|        |           cat |       1.287 |               3337 |
|        |          fcat |       0.480 |               8948 |

| Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
|--------|---------------|-------------|--------------------|
|     gz |          gzip |      18.806 |                228 |
|     gz |         bgzip |       4.170 |               1030 |
|     gz |   bgzip -@ 24 |       4.087 |               1051 |
|     gz |          pigz |       4.391 |                978 |
|     gz |         igzip |       2.635 |               1630 |
|     gz |   igzip -T 24 |       2.573 |               1669 |
|     gz |  rapidgzip -P 1 |       1.244 |               3453 |
|     gz | rapidgzip -P 24 |       1.618 |               2654 |

| Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
|--------|---------------|-------------|--------------------|
|    bgz |          gzip |      20.119 |                213 |
|    bgz |         bgzip |       1.792 |               2397 |
|    bgz |   bgzip -@ 24 |       1.919 |               2238 |
|    bgz |          pigz |       9.072 |                473 |
|    bgz |         igzip |       1.775 |               2420 |
|    bgz |   igzip -T 24 |       1.733 |               2478 |
|    bgz |  rapidgzip -P 1 |       1.932 |               2223 |
|    bgz | rapidgzip -P 24 |       1.589 |               2703 |

| Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
|--------|---------------|-------------|--------------------|
|   pigz |          gzip |      19.810 |                217 |
|   pigz |         bgzip |       4.005 |               1072 |
|   pigz |   bgzip -@ 24 |       4.038 |               1064 |
|   pigz |          pigz |       4.062 |               1057 |
|   pigz |         igzip |       2.500 |               1718 |
|   pigz |   igzip -T 24 |       2.335 |               1839 |
|   pigz |  rapidgzip -P 1 |       1.027 |               4182 |
|   pigz | rapidgzip -P 24 |       1.618 |               2654 |

| Format |       Decoder | Runtime / s | Bandwidth / (MB/s) |
|--------|---------------|-------------|--------------------|
|    igz |          gzip |      26.927 |                160 |
|    igz |         bgzip |       5.601 |                767 |
|    igz |   bgzip -@ 24 |       5.446 |                789 |
|    igz |          pigz |       5.194 |                827 |
|    igz |         igzip |       3.839 |               1119 |
|    igz |   igzip -T 24 |       3.888 |               1105 |
|    igz |  rapidgzip -P 1 |       4.548 |                944 |
|    igz | rapidgzip -P 24 |       1.479 |               2904 |



# Benchmark rapidgzip scaling over threads

for fname in 4GiB-base64 4GiB-random; do
    fileSize=$( stat -L --format=%s -- "$fname" )
    format=gz
    printf '\n| %14s | %13s | %10s | %18s |\n' File Decoder 'Runtime / s' 'Bandwidth / (MB/s)'
    printf -- '|----------------|---------------|-------------|--------------------|\n'
    crc32 "$fname.$format" &>/dev/null  # Make my QLC SSD cache the file into the SLC cache

    for parallelization in 1 2 4 8 12 16 24 32; do
        tool="src/tools/rapidgzip -P $parallelization"
        visibleTool="rapidgzip -P $parallelization"
        runtime=$( ( time $tool -d -c "$fname.$format" | wc -c ) 2>&1 | sed -n -E 's|real[ \t]*0m||p' | sed 's|[ \ts]||' )
        bandwidth=$( python3 -c "print( int( round( $fileSize / 1e6 / $runtime ) ) )" )
        printf '| %14s | %13s | %11s | %18s |\n' "$fname.$format" "$visibleTool" "$runtime" "$bandwidth"
    done
done

    |           File |       Decoder | Runtime / s | Bandwidth / (MB/s) |
    |----------------|---------------|-------------|--------------------|
    | 4GiB-base64.gz |  rapidgzip -P 1 |      19.763 |                217 |
    | 4GiB-base64.gz |  rapidgzip -P 2 |       9.782 |                439 |
    | 4GiB-base64.gz |  rapidgzip -P 4 |       5.122 |                839 |
    | 4GiB-base64.gz |  rapidgzip -P 8 |       2.905 |               1478 |
    | 4GiB-base64.gz | rapidgzip -P 12 |       2.241 |               1917 |
    | 4GiB-base64.gz | rapidgzip -P 16 |       1.890 |               2272 |
    | 4GiB-base64.gz | rapidgzip -P 24 |       1.703 |               2522 |
    | 4GiB-base64.gz | rapidgzip -P 32 |       1.716 |               2503 |

    |           File |       Decoder | Runtime / s | Bandwidth / (MB/s) |
    |----------------|---------------|-------------|--------------------|
    | 4GiB-random.gz |  rapidgzip -P 1 |       1.289 |               3332 |
    | 4GiB-random.gz |  rapidgzip -P 2 |       1.961 |               2190 |
    | 4GiB-random.gz |  rapidgzip -P 4 |       1.431 |               3001 |
    | 4GiB-random.gz |  rapidgzip -P 8 |       1.276 |               3366 |
    | 4GiB-random.gz | rapidgzip -P 12 |       1.306 |               3289 |
    | 4GiB-random.gz | rapidgzip -P 16 |       1.439 |               2985 |
    | 4GiB-random.gz | rapidgzip -P 24 |       1.509 |               2846 |
    | 4GiB-random.gz | rapidgzip -P 32 |       1.530 |               2807 |
*/
