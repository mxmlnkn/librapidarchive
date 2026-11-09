#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include <core/BitManipulation.hpp>


using namespace rapidgzip;


namespace
{
[[nodiscard]] inline uint64_t
byteSwapPrevious( uint64_t value ) noexcept
{
    value = ( ( value & uint64_t( 0x0000'0000'FFFF'FFFFULL ) ) << 32U ) |
            ( ( value & uint64_t( 0xFFFF'FFFF'0000'0000ULL ) ) >> 32U );
    value = ( ( value & uint64_t( 0x0000'FFFF'0000'FFFFULL ) ) << 16U ) |
            ( ( value & uint64_t( 0xFFFF'0000'FFFF'0000ULL ) ) >> 16U );
    value = ( ( value & uint64_t( 0x00FF'00FF'00FF'00FFULL ) ) << 8U  ) |
            ( ( value & uint64_t( 0xFF00'FF00'FF00'FF00ULL ) ) >> 8U  );
    return value;
}


[[nodiscard]] inline uint64_t
byteSwapRefillFallback( uint64_t value ) noexcept
{
    auto result = ( ( value & 0x00FF00FF00FF00FFULL ) << 8U ) | ( ( value >> 8U ) & 0x00FF00FF00FF00FFULL );
    result = ( ( result & 0x0000FFFF0000FFFFULL ) << 16U ) | ( ( result >> 16U ) & 0x0000FFFF0000FFFFULL );
    return ( result << 32U ) | ( result >> 32U );
}


[[nodiscard]] inline uint64_t
byteSwapBuiltin( uint64_t value ) noexcept
{
#if defined( __GNUC__ ) || defined( __clang__ )
    return __builtin_bswap64( value );
#elif defined( _MSC_VER )
    return _byteswap_uint64( value );
#else
    return byteSwapPrevious( value );
#endif
}


volatile uint64_t benchmarkSink{ 0 };


template<typename Function>
[[nodiscard]] double
measure( Function&&                    function,
         const std::vector<uint64_t>&  values,
         const size_t                  repeats )
{
    uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for ( size_t repeat = 0; repeat < repeats; ++repeat ) {
        for ( const auto value : values ) {
            sum += function( value );
        }
    }
    const auto duration = std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
    benchmarkSink = sum;
    return static_cast<double>( values.size() ) * static_cast<double>( repeats ) / duration / 1e9;
}


[[nodiscard]] double
median( std::vector<double> values )
{
    std::sort( values.begin(), values.end() );
    return values[values.size() / 2];
}
}  // namespace


int
main()
{
    constexpr size_t VALUE_COUNT = 1U << 20U;
    constexpr size_t REPEATS = 64;
    constexpr size_t ROUNDS = 5;
    constexpr size_t VARIANT_COUNT = 4;

    std::mt19937_64 randomEngine( 0x1234567890ABCDEFULL );
    std::vector<uint64_t> values( VALUE_COUNT );
    for ( auto& value : values ) {
        value = randomEngine();
    }

    const auto currentByteSwap = [] ( const uint64_t value ) { return byteSwap( value ); };
    const std::array<const char*, VARIANT_COUNT> names{ "previous", "refill fallback", "builtin", "current core" };
    std::array<std::vector<double>, VARIANT_COUNT> samples;

    for ( size_t round = 0; round < ROUNDS; ++round ) {
        for ( size_t i = 0; i < VARIANT_COUNT; ++i ) {
            const auto variant = ( i + round ) % VARIANT_COUNT;
            switch ( variant ) {
            case 0:
                samples[0].push_back( measure( byteSwapPrevious, values, REPEATS ) );
                break;
            case 1:
                samples[1].push_back( measure( byteSwapRefillFallback, values, REPEATS ) );
                break;
            case 2:
                samples[2].push_back( measure( byteSwapBuiltin, values, REPEATS ) );
                break;
            case 3:
                samples[3].push_back( measure( currentByteSwap, values, REPEATS ) );
                break;
            }
        }
    }

    for ( size_t variant = 0; variant < VARIANT_COUNT; ++variant ) {
        std::cout << names[variant] << ": " << median( samples[variant] ) << " G values/s\n";
    }
    std::cout << "sink " << benchmarkSink << "\n";
    return 0;
}
