/**
 * Isolated benchmark for bzip2's non-reflected CRC32 (polynomial 0x04C11DB7) with 1, 4, and 8 slices.
 * All variants must produce the same checksum. The reported bandwidth is the minimum over several runs.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <core/DataGenerators.hpp>
#include <indexed_bzip2/bzip2.hpp>


namespace
{
using Clock = std::chrono::steady_clock;

constexpr size_t DATA_SIZE = 128U * 1024U * 1024U + 13U;
constexpr size_t REPEAT_COUNT = 5;


template<unsigned int SLICE_SIZE>
[[nodiscard]] uint32_t
checksum( const std::vector<uint8_t>& data )
{
    return ~bzip2::updateCRC32SliceByN<SLICE_SIZE>( ~uint32_t( 0 ), data.data(), data.size() );
}


template<unsigned int SLICE_SIZE>
void
benchmark( const std::vector<uint8_t>& data,
           const uint32_t              expected )
{
    double bestSeconds = std::numeric_limits<double>::max();
    uint32_t result = 0;
    for ( size_t repeat = 0; repeat < REPEAT_COUNT; ++repeat ) {
        const auto start = Clock::now();
        result = checksum<SLICE_SIZE>( data );
        const auto stop = Clock::now();
        const auto seconds = std::chrono::duration<double>( stop - start ).count();
        bestSeconds = std::min( bestSeconds, seconds );
    }

    if ( result != expected ) {
        std::cerr << "CRC mismatch for " << SLICE_SIZE << " slices: 0x" << std::hex << result
                  << " != 0x" << expected << std::dec << "\n";
        std::exit( 1 );
    }

    const auto gigabytesPerSecond = static_cast<double>( data.size() ) / bestSeconds / 1e9;
    std::cout << "slices=" << SLICE_SIZE << "  "
              << std::fixed << std::setprecision( 3 ) << gigabytesPerSecond << " GB/s"
              << "  (min of " << REPEAT_COUNT << ", checksum 0x" << std::hex << std::uppercase << result
              << std::dec << ")\n";
}
}  // namespace


int
main()
{
    std::cout << "Generating " << DATA_SIZE / ( 1024U * 1024U ) << " MiB of test data ... " << std::flush;
    std::vector<uint8_t> data( DATA_SIZE );
    rapidgzip::fillWithRandomData( data.data(), data.size() );
    std::cout << "done\n";

    const auto expected = checksum<1>( data );
    benchmark<1>( data, expected );
    benchmark<4>( data, expected );
    benchmark<8>( data, expected );
    return 0;
}
