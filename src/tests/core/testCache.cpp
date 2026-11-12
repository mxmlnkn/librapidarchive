#include <iostream>

#include <core/Cache.hpp>
#include <core/TestHelpers.hpp>


using namespace rapidgzip;


void
testCacheReinsertion()
{
    Cache<size_t, double> cache( /* capacity */ 2 );

    cache.insert( 2, 4.0 );
    cache.insert( 1, 1.0 );
    /* Replacing an existing key's value (buffers with markers replaced) should not trigger evictions. */
    cache.insert( 1, 2.0 );

    /** Unused entries are those that got evicted without them being accessed first. */
    REQUIRE_EQUAL( cache.statistics().unusedEntries, 0U );
}


void
testCacheClearResetsStrategy()
{
    Cache<size_t, double> cache( /* capacity */ 1 );
    cache.insert( 10, 1.0 );
    cache.clear();
    cache.insert( 20, 2.0 );

    /* The cleared key must not remain in the eviction strategy, otherwise it would be reported as the next
     * eviction although it is not in the cache anymore. */
    REQUIRE( cache.cacheStrategy().nextEviction() == std::optional<size_t>{ 20 } );
}


int
main()
{
    testCacheReinsertion();
    testCacheClearResetsStrategy();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
