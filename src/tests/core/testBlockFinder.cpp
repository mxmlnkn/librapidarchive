#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include <core/BlockFinder.hpp>
#include <core/ParallelBitStringFinder.hpp>
#include <core/TestHelpers.hpp>
#include <core/common.hpp>


using namespace rapidgzip;


int
main()
{
    /* Enough matches so that the finder does not finalize before the restart. */
    constexpr size_t MATCH_COUNT = 100'000;
    const std::vector<uint8_t> buffer( MATCH_COUNT, 0xAA );
    constexpr uint8_t BIT_STRING_SIZE = 8;

    using RawFinder = ParallelBitStringFinder<BIT_STRING_SIZE>;
    auto rawFinder = std::make_unique<RawFinder>(
        reinterpret_cast<const char*>( buffer.data() ), buffer.size(), 0xAA, /* parallelization */ 2 );
    BlockFinder<RawFinder> blockFinder( std::move( rawFinder ) );

    const auto first = blockFinder.get( 0, 10 );
    REQUIRE( first.second == BlockFinderInterface::GetReturnCode::SUCCESS );
    REQUIRE( first.first.has_value() );

    /* stopThreads() must join the worker, stop the raw finder pool, and allow a later get() to restart. */
    blockFinder.stopThreads();

    /* Requested block is far beyond the prefetch distance, so this must restart the finder. */
    const auto restarted = blockFinder.get( 50'000, 10 );
    REQUIRE( restarted.second == BlockFinderInterface::GetReturnCode::SUCCESS );
    REQUIRE( restarted.first.has_value() );
    REQUIRE( *restarted.first > *first.first );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
