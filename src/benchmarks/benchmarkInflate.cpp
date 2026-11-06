/**
 * @file Executes benchmarks for varying gzip test files using the custom-written sequential deflate decompressor.
 *       This should yield a wide variety of timings that can be used to optimize the inflate hot-loop.
 */

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/common.hpp>
#include <core/Statistics.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/GzipReader.hpp>
#include <rapidgzip/gzip/zlib.hpp>


using namespace rapidgzip;


struct BenchmarkResult
{
    std::string name;
    size_t size{ 0 };
    std::vector<double> durations;
};


[[nodiscard]] size_t
decompressWithRapidgzip( UniqueFileReader fileReader )
{
    using namespace rapidgzip;

    size_t totalDecodedBytes = 0;
    [[maybe_unused]] size_t blockCount = 0;

    GzipReader gzipReader( std::move( fileReader ) );
    /* The inputs are generated and compressed by this benchmark, i.e., they are trusted.
     * Disable CRC verification to measure pure inflate throughput.
     * CRC32 is a separate cost: callgrind snapshot: ~37M of 267M instructions in the sequential path. */
    gzipReader.setCRC32Enabled( true );
    std::vector<uint8_t> outputBuffer( 64_Mi );
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

    return totalDecodedBytes;
}


void
printBandwidths( const std::vector<double>& durations,
                 size_t                     nBytesDecoded )
{
    std::cout << "    Runtime / s: " << Statistics<double>( durations ).formatAverageWithUncertainty( true ) << "\n";

    std::vector<double> decodedBandwidths( durations.size() );
    std::transform( durations.begin(), durations.end(), decodedBandwidths.begin(),
                    [nBytesDecoded] ( auto duration ) {
                        return static_cast<double>( nBytesDecoded ) / 1e6 / duration;
                    } );
    std::cout << "    Bandwidth on Decoded Data / (MB/s): "
              << Statistics<double>( decodedBandwidths ).formatAverageWithUncertainty( true ) << "\n";
};


BenchmarkResult
benchmarkDecompression( const std::vector<std::byte>& dataToCompress,
                        std::string                   name,
                        const CompressionStrategy     compressionStrategy = CompressionStrategy::DEFAULT )
{
    const auto t0 = now();
    const auto fileContents = compressWithZlib( dataToCompress, compressionStrategy );
    const auto compressDuration = duration( t0 );
    const auto compressionRatio = static_cast<double>( dataToCompress.size() )
                                  / static_cast<double>( fileContents.size() );
    const auto compressionBandwidth = static_cast<double>( dataToCompress.size() ) / compressDuration;

    std::cout << "Compressed " << formatBytes( dataToCompress.size() )
              << " " << name << " to " << formatBytes( fileContents.size() )
              << ", compression ratio: " << compressionRatio
              << ",\n  compression strategy: " << toString( compressionStrategy )
              << ", compression bandwidth: " << compressionBandwidth / 1e6 << " MB/s\n";

    auto [size, durations] = benchmarkFunction<3>(
        [&fileContents] () {
            return decompressWithRapidgzip( std::make_unique<BufferViewFileReader>( fileContents ) );
        } );
    printBandwidths( durations, size );
    std::cout << "\n";
    return { std::move( name ), size, std::move( durations ) };
}


BenchmarkResult
benchmarkDecompressionOfZeros()
{
    return benchmarkDecompression( std::vector<std::byte>( 128_Mi, std::byte( 0 ) ), "Zeros" );
}


[[nodiscard]] std::vector<std::byte>
createRandomData( uint64_t size )
{
    std::mt19937_64 randomEngine;
    std::vector<std::byte> result( size );
    for ( auto& x : result ) {
        x = static_cast<std::byte>( randomEngine() );
    }
    return result;
}


[[nodiscard]] std::vector<std::byte>
createRandomData( uint64_t                      size,
                  const std::vector<std::byte>& allowedSymbols )
{
    std::mt19937_64 randomEngine;
    std::vector<std::byte> result( size );
    for ( auto& x : result ) {
        x = allowedSymbols[static_cast<size_t>( randomEngine() ) % allowedSymbols.size()];
    }
    return result;
}


BenchmarkResult
benchmarkDecompressionOfNonCompressible()
{
    const auto t0 = now();
    const auto randomData = createRandomData( 128_Mi );
    const auto creationDuration = duration( t0 );
    std::cout << "Created " << formatBytes( randomData.size() ) << " random data in " << creationDuration << " s\n";
    return benchmarkDecompression( randomData, "Random", CompressionStrategy::HUFFMAN_ONLY );
}


BenchmarkResult
benchmarkDecompressionOfRandomBase64( const CompressionStrategy compressionStrategy )
{
    std::vector<std::byte> allowedSymbols( BASE64_SYMBOLS.size() );
    std::transform( BASE64_SYMBOLS.begin(), BASE64_SYMBOLS.end(), allowedSymbols.begin(),
                    [] ( const auto c ) { return static_cast<std::byte>( c ); } );

    const auto t0 = now();
    const auto randomData = createRandomData( 128_Mi, allowedSymbols );
    const auto creationDuration = duration( t0 );
    std::cout << "Created " << formatBytes( randomData.size() )
              << " random base64 data in " << creationDuration << " s\n";
    const auto name = compressionStrategy == CompressionStrategy::FIXED_HUFFMAN
                      ? std::string( "Random Base64 (Fixed Huffman)" )
                      : std::string( "Random Base64" );
    return benchmarkDecompression( randomData, name, compressionStrategy );
}


/**
 * Generates matches that overlap their source (length > distance). Such matches repeat a short period and are
 * decoded by the repetition copy path in resolveBackreference, unlike the random-backreference case, which
 * rejects length > distance and therefore only covers the non-overlapping copy paths.
 */
BenchmarkResult
benchmarkDecompressionOfOverlappingBackreferences()
{
    const auto t0 = now();

    std::mt19937_64 randomEngine;

    constexpr auto INITIAL_RANDOM_SIZE = rapidgzip::deflate::MAX_WINDOW_SIZE;
    auto randomData = createRandomData( INITIAL_RANDOM_SIZE );
    randomData.resize( 128_Mi );

    for ( size_t i = INITIAL_RANDOM_SIZE; i < randomData.size(); ) {
        const auto distance = 2 + randomEngine() % 30;
        const auto remainingSize = randomData.size() - i;
        const auto length = std::min<size_t>( 4 + randomEngine() % 255, remainingSize );
        /* The source and destination overlap when length > distance. memcpy is undefined in that case and
         * memmove would copy the pre-existing bytes instead of the repeated LZ77 pattern. */
        for ( size_t k = 0; k < length; ++k ) {
            randomData[i + k] = randomData[i + k - distance];
        }
        i += length;
    }

    const auto creationDuration = duration( t0 );
    std::cout << "Created " << formatBytes( randomData.size() )
              << " data with overlapping backreferences in " << creationDuration << " s\n";
    return benchmarkDecompression( randomData, "Random Overlapping Backreferences" );
}


BenchmarkResult
benchmarkDecompressionOfRandomBackreferences()
{
    const auto t0 = now();

    std::mt19937_64 randomEngine;

    constexpr auto INITIAL_RANDOM_SIZE = rapidgzip::deflate::MAX_WINDOW_SIZE;
    auto randomData = createRandomData( INITIAL_RANDOM_SIZE );
    randomData.resize( 128_Mi );

    for ( size_t i = INITIAL_RANDOM_SIZE; i < randomData.size(); ) {
        const auto distance = randomEngine() % INITIAL_RANDOM_SIZE;
        const auto remainingSize = randomData.size() - i;
        const auto length = std::min<size_t>( randomEngine() % 256, remainingSize );
        if ( ( length < 4 ) || ( length > distance ) ) {
            continue;
        }

        assert( length <= distance );
        std::memcpy( randomData.data() + i, randomData.data() + ( i - distance ), length );
        i += length;
    }

    const auto creationDuration = duration( t0 );
    std::cout << "Created " << formatBytes( randomData.size() )
              << " data with random backreferences in " << creationDuration << " s\n";
    return benchmarkDecompression( randomData, "Random Backreferences" );
}


void
printSummaryTable( const std::vector<BenchmarkResult>& results )
{
#ifdef LIBRAPIDARCHIVE_WITH_ISAL
    constexpr auto isalLabel = "WITH_ISAL=ON";
#else
    constexpr auto isalLabel = "WITH_ISAL=OFF";
#endif

    constexpr int bandwidthWidth = 31;
    auto nameWidth = std::string_view( "Data Type" ).size();
    for ( const auto& result : results ) {
        nameWidth = std::max( nameWidth, result.name.size() );
    }

    const auto header = std::string( "Bandwidth (MB/s) " ) + isalLabel;

    std::cout << "| " << std::left << std::setw( static_cast<int>( nameWidth ) ) << "Data Type" << " | "
              << std::left << std::setw( bandwidthWidth ) << header << " |\n";
    std::cout << "|" << std::string( nameWidth + 2, '-' ) << "|"
              << std::string( bandwidthWidth + 2, '-' ) << "|\n";

    for ( const auto& result : results ) {
        std::vector<double> bandwidths( result.durations.size() );
        std::transform( result.durations.begin(), result.durations.end(), bandwidths.begin(),
                        [size = result.size] ( const auto duration ) {
                            return static_cast<double>( size ) / 1e6 / duration;
                        } );
        const auto statistics = Statistics<double>( bandwidths ).formatAverageWithUncertainty( false );
        std::cout << "| " << std::left << std::setw( static_cast<int>( nameWidth ) ) << result.name << " | "
                  << std::left << std::setw( bandwidthWidth ) << statistics << " |\n";
    }
}


int
main()
{
    std::vector<BenchmarkResult> results;
    results.emplace_back( benchmarkDecompressionOfZeros() );
    results.emplace_back( benchmarkDecompressionOfNonCompressible() );
    results.emplace_back( benchmarkDecompressionOfRandomBase64( CompressionStrategy::HUFFMAN_ONLY ) );
    results.emplace_back( benchmarkDecompressionOfRandomBase64( CompressionStrategy::FIXED_HUFFMAN ) );
    results.emplace_back( benchmarkDecompressionOfRandomBackreferences() );
    results.emplace_back( benchmarkDecompressionOfOverlappingBackreferences() );

    std::cout << "\n";
    printSummaryTable( results );
    return 0;
}


/*
cmake -DLIBRAPIDARCHIVE_BUILD_BENCHMARKS=ON -DLIBRAPIDARCHIVE_WITH_ISAL=OFF ..
cmake --build . -- benchmarkInflate && taskset 0x01 src/benchmarks/benchmarkInflate

# LIBRAPIDARCHIVE_WITH_ISAL=OFF

Compressed 128 MiB Zeros to 127 KiB 430 B, compression ratio: 1028.66,
  compression strategy: Default, compression bandwidth: 2294.42 MB/s
    Runtime / s: 0.0742 <= 0.0749 +- 0.0008 <= 0.0757
    Bandwidth on Decoded Data / (MB/s): 1773 <= 1791 +- 18 <= 1810

Created 128 MiB random data in 0.412546 s
Compressed 128 MiB Random to 128 MiB 40 KiB 23 B, compression ratio: 0.999695,
  compression strategy: Huffman Only, compression bandwidth: 306.929 MB/s
    Runtime / s: 0.08175 <= 0.08208 +- 0.00029 <= 0.08228
    Bandwidth on Decoded Data / (MB/s): 1631 <= 1635 +- 6 <= 1642

Created 128 MiB random base64 data in 0.428251 s
Compressed 128 MiB Random Base64 to 96 MiB 400 KiB 19 B, compression ratio: 1.32793,
  compression strategy: Huffman Only, compression bandwidth: 295.008 MB/s
    Runtime / s: 0.5024 <= 0.5040 +- 0.0020 <= 0.5063
    Bandwidth on Decoded Data / (MB/s): 265.1 <= 266.3 +- 1.1 <= 267.2

Created 128 MiB random base64 data in 0.432856 s
Compressed 128 MiB Random Base64 (Fixed Huffman) to 127 MiB 782 KiB 93 B, compression ratio: 1.00185,
  compression strategy: Fixed Huffman, compression bandwidth: 70.5349 MB/s
    Runtime / s: 0.496 <= 0.513 +- 0.029 <= 0.547
    Bandwidth on Decoded Data / (MB/s): 245 <= 262 +- 14 <= 270

Created 128 MiB data with random backreferences in 0.0719573 s
Compressed 128 MiB Random Backreferences to 3 MiB 676 KiB 270 B, compression ratio: 34.9687,
  compression strategy: Default, compression bandwidth: 280.434 MB/s
    Runtime / s: 0.0957 <= 0.0962 +- 0.0005 <= 0.0966
    Bandwidth on Decoded Data / (MB/s): 1390 <= 1395 +- 7 <= 1402

Created 128 MiB data with overlapping backreferences in 0.0712416 s
Compressed 128 MiB Random Overlapping Backreferences to 159 KiB 413 B, compression ratio: 822.266,
  compression strategy: Default, compression bandwidth: 2311.78 MB/s
    Runtime / s: 0.07532 <= 0.07542 +- 0.00011 <= 0.07553
    Bandwidth on Decoded Data / (MB/s): 1777.0 <= 1779.6 +- 2.5 <= 1781.9


# LIBRAPIDARCHIVE_WITH_ISAL=ON

Compressed 128 MiB Zeros to 127 KiB 430 B, compression ratio: 1028.66,
  compression strategy: Default, compression bandwidth: 2333.83 MB/s
    Runtime / s: 0.0493 <= 0.0497 +- 0.0006 <= 0.0504
    Bandwidth on Decoded Data / (MB/s): 2660 <= 2700 +- 30 <= 2730

Created 128 MiB random data in 0.415043 s
Compressed 128 MiB Random to 128 MiB 40 KiB 23 B, compression ratio: 0.999695,
  compression strategy: Huffman Only, compression bandwidth: 298.75 MB/s
    Runtime / s: 0.05404 <= 0.05411 +- 0.00007 <= 0.05418
    Bandwidth on Decoded Data / (MB/s): 2477 <= 2481 +- 3 <= 2484

Created 128 MiB random base64 data in 0.429297 s
Compressed 128 MiB Random Base64 to 96 MiB 400 KiB 19 B, compression ratio: 1.32793,
  compression strategy: Huffman Only, compression bandwidth: 283.886 MB/s
    Runtime / s: 0.463 <= 0.469 +- 0.007 <= 0.477
    Bandwidth on Decoded Data / (MB/s): 281 <= 286 +- 4 <= 290

Created 128 MiB random base64 data in 0.429803 s
Compressed 128 MiB Random Base64 (Fixed Huffman) to 127 MiB 782 KiB 93 B, compression ratio: 1.00185,
  compression strategy: Fixed Huffman, compression bandwidth: 69.1795 MB/s
    Runtime / s: 0.4780 <= 0.4800 +- 0.0023 <= 0.4825
    Bandwidth on Decoded Data / (MB/s): 278.2 <= 279.6 +- 1.3 <= 280.8

Created 128 MiB data with random backreferences in 0.0717709 s
Compressed 128 MiB Random Backreferences to 3 MiB 676 KiB 270 B, compression ratio: 34.9687,
  compression strategy: Default, compression bandwidth: 281.146 MB/s
    Runtime / s: 0.0681 <= 0.0685 +- 0.0006 <= 0.0691
    Bandwidth on Decoded Data / (MB/s): 1941 <= 1960 +- 17 <= 1970

Created 128 MiB data with overlapping backreferences in 0.0728748 s
Compressed 128 MiB Random Overlapping Backreferences to 159 KiB 413 B, compression ratio: 822.266,
  compression strategy: Default, compression bandwidth: 2312.87 MB/s
    Runtime / s: 0.04986 <= 0.05000 +- 0.00013 <= 0.05013
    Bandwidth on Decoded Data / (MB/s): 2678 <= 2684 +- 7 <= 2692


| Data Type                         | Bandwidth (MB/s) WITH_ISAL=OFF  | Bandwidth (MB/s) WITH_ISAL=ON   |
|-----------------------------------|---------------------------------|---------------------------------|
| Zeros                             | 1791 +- 18                      | 2700 +- 30                      |
| Random                            | 1635 +- 6                       | 2481 +- 3                       |
| Random Base64                     | 266.3 +- 1.1                    | 286 +- 4                        |
| Random Base64 (Fixed Huffman)     | 262 +- 14                       | 279.6 +- 1.3                    |
| Random Backreferences             | 1395 +- 7                       | 1960 +- 17                      |
| Random Overlapping Backreferences | 1779.6 +- 2.5                   | 2684 +- 7                       |



uname -a
    Linux raspberrypi5 6.8.0-1028-raspi #32-Ubuntu SMP PREEMPT_DYNAMIC aarch64 aarch64 aarch64 GNU/Linux
cmake --build . -- benchmarkInflate && src/benchmarks/benchmarkInflate
LIBRAPIDARCHIVE_WITH_ISAL=ON

Compressed 128 MiB zeros to 127 KiB 430 B, compression ratio: 1028.66,
  compression strategy: Default, compression bandwidth: 154.11 MB/s
    Runtime / s: 0.1165 <= 0.1176 +- 0.0011 <= 0.1187
    Bandwidth on Decoded Data / (MB/s): 1131 <= 1142 +- 11 <= 1152

Created 128 MiB random data in 0.775585 s
Compressed 128 MiB random data to 128 MiB 40 KiB 23 B, compression ratio: 0.999695,
  compression strategy: Huffman Only, compression bandwidth: 58.1063 MB/s
    Runtime / s: 0.106 <= 0.108 +- 0.004 <= 0.113
    Bandwidth on Decoded Data / (MB/s): 1190 <= 1240 +- 40 <= 1270

Created 128 MiB random base64 data in 0.924664 s
Compressed 128 MiB random base64 data to 96 MiB 400 KiB 37 B, compression ratio: 1.32793,
  compression strategy: Huffman Only, compression bandwidth: 56.1636 MB/s
    Runtime / s: 0.799 <= 0.802 +- 0.003 <= 0.805
    Bandwidth on Decoded Data / (MB/s): 166.6 <= 167.3 +- 0.7 <= 168.0

Created 128 MiB random base64 data in 0.988831 s
Compressed 128 MiB random base64 data to 127 MiB 73 KiB 255 B, compression ratio: 1.00731,
  compression strategy: Fixed Huffman, compression bandwidth: 19.3699 MB/s
    Runtime / s: 1.08 <= 1.11 +- 0.04 <= 1.15
    Bandwidth on Decoded Data / (MB/s): 117 <= 121 +- 4 <= 124

Created 128 MiB data with random backreferences in 0.266884 s
Compressed 128 MiB data with random backreferences to 5 MiB 736 KiB 719 B, compression ratio: 22.3798,
  compression strategy: Default, compression bandwidth: 42.5468 MB/s
    Runtime / s: 0.206 <= 0.225 +- 0.028 <= 0.257
    Bandwidth on Decoded Data / (MB/s): 520 <= 600 +- 70 <= 650


LIBRAPIDARCHIVE_WITH_ISAL=OFF

Compressed 128 MiB zeros to 127 KiB 430 B, compression ratio: 1028.66,
  compression strategy: Default, compression bandwidth: 164.403 MB/s
    Runtime / s: 0.2077 <= 0.2087 +- 0.0008 <= 0.2093
    Bandwidth on Decoded Data / (MB/s): 641.2 <= 643.2 +- 2.6 <= 646.1

Created 128 MiB random data in 0.772814 s
Compressed 128 MiB random data to 128 MiB 40 KiB 23 B, compression ratio: 0.999695,
  compression strategy: Huffman Only, compression bandwidth: 57.0674 MB/s
    Runtime / s: 0.1936 <= 0.1961 +- 0.0029 <= 0.1992
    Bandwidth on Decoded Data / (MB/s): 674 <= 685 +- 10 <= 693

Created 128 MiB random base64 data in 0.921788 s
Compressed 128 MiB random base64 data to 96 MiB 400 KiB 37 B, compression ratio: 1.32793,
  compression strategy: Huffman Only, compression bandwidth: 56.91 MB/s
    Runtime / s: 1.08 <= 1.11 +- 0.03 <= 1.14
    Bandwidth on Decoded Data / (MB/s): 118 <= 121 +- 3 <= 124

Created 128 MiB random base64 data in 0.92377 s
Compressed 128 MiB random base64 data to 127 MiB 73 KiB 255 B, compression ratio: 1.00731,
  compression strategy: Fixed Huffman, compression bandwidth: 19.7438 MB/s
    Runtime / s: 1.17 <= 1.21 +- 0.04 <= 1.23
    Bandwidth on Decoded Data / (MB/s): 109 <= 111 +- 3 <= 115

Created 128 MiB data with random backreferences in 0.288451 s
Compressed 128 MiB data with random backreferences to 5 MiB 736 KiB 719 B, compression ratio: 22.3798,
  compression strategy: Default, compression bandwidth: 41.651 MB/s
    Runtime / s: 0.3004 <= 0.3016 +- 0.0012 <= 0.3028
    Bandwidth on Decoded Data / (MB/s): 443.2 <= 445.0 +- 1.8 <= 446.9

| Data Type                     | Bandwidth (MB/s) WITH_ISAL=OFF | Bandwidth (MB/s) WITH_ISAL=ON |
|-------------------------------|--------------------------------|-------------------------------|
| Zeros                         | 643.2 +- 2.6                   | 1142   +- 11                  |
| Random                        | 685   +- 10                    | 1240   +- 40                  |
| Random Base64                 | 121   +- 3                     |  167.3 +- 0.7                 |
| Random Base64 (Fixed Huffman) | 111   +- 3                     |  121   +- 4                   |
| Random Backreferences         | 445.0 +- 1.8                   |  600   +- 70                  |
*/
