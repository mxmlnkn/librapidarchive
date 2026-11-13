#include <iostream>
#include <stdexcept>

#include <core/common.hpp>
#include <core/BlockMap.hpp>
#include <core/TestHelpers.hpp>


using namespace rapidgzip;


void
testFinalizeEmptyMap()
{
    BlockMap blockMap;
    REQUIRE( blockMap.empty() );
    REQUIRE_EQUAL( blockMap.dataBlockCount(), size_t( 0 ) );

    blockMap.finalize();
    REQUIRE( blockMap.finalized() );
    REQUIRE_EQUAL( blockMap.dataBlockCount(), size_t( 0 ) );
    const auto offsets = blockMap.blockOffsets();
    REQUIRE_EQUAL( offsets.size(), size_t( 1 ) );
    REQUIRE_EQUAL( offsets.begin()->first, size_t( 0 ) );
    REQUIRE_EQUAL( offsets.begin()->second, size_t( 0 ) );
}


void
testEmptyImportedOffsetsAreRejected()
{
    BlockMap blockMap;
    bool threw{ false };
    try {
        blockMap.setBlockOffsets( {} );
    } catch ( const std::invalid_argument& ) {
        threw = true;
    }
    REQUIRE( threw );
}


void
testPushAndFinalize()
{
    BlockMap blockMap;
    REQUIRE_EQUAL( blockMap.push( 32, 800, 1000 ), size_t( 0 ) );
    REQUIRE_EQUAL( blockMap.push( 832, 800, 0 ), size_t( 1000 ) );
    REQUIRE_EQUAL( blockMap.dataBlockCount(), size_t( 1 ) );

    blockMap.finalize();
    REQUIRE( blockMap.finalized() );
    /* finalize() appends the implied end sentinel so that the last real block's size is known. */
    REQUIRE_EQUAL( blockMap.dataBlockCount(), size_t( 1 ) );
    const auto offsets = blockMap.blockOffsets();
    REQUIRE_EQUAL( offsets.size(), size_t( 3 ) );
}


int
main()
{
    testFinalizeEmptyMap();
    testEmptyImportedOffsetsAreRejected();
    testPushAndFinalize();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
