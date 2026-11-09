#include <iostream>
#include <memory>

#include <core/ReusableVector.hpp>
#include <core/TestHelpers.hpp>


using namespace rapidgzip;


int
main()
{
    /* A fresh pool has no traffic and no free buffers. */
    {
        ReusableBufferPool<uint8_t> pool;
        pool.setStatisticsEnabled( true );
        const auto stats = pool.statistics();
        REQUIRE_EQUAL( stats.acquireCalls, 0U );
        REQUIRE_EQUAL( stats.acquireHits, 0U );
        REQUIRE_EQUAL( stats.acquireMisses, 0U );
        REQUIRE_EQUAL( stats.releaseCalls, 0U );
        REQUIRE_EQUAL( stats.freeBufferCount, 0U );
        REQUIRE_EQUAL( stats.freeBufferBytes, 0U );
    }

    /* A miss allocates through ReusableVector, and the released buffer is retained. */
    {
        auto pool = std::make_shared<ReusableBufferPool<uint8_t> >();
        pool->setStatisticsEnabled( true );
        {
            ReusableVector<uint8_t> vector( pool );
            vector.resize( 100 );
            REQUIRE_EQUAL( vector.size(), 100U );
            for ( size_t i = 0; i < vector.size(); ++i ) {
                vector.data()[i] = static_cast<uint8_t>( i );
            }

            const auto stats = pool->statistics();
            REQUIRE_EQUAL( stats.acquireCalls, 1U );
            REQUIRE_EQUAL( stats.acquireMisses, 1U );
            REQUIRE_EQUAL( stats.acquireHits, 0U );
            REQUIRE_EQUAL( stats.allocatedBytes, 4096U );
            REQUIRE_EQUAL( stats.freeBufferCount, 0U );
        }

        const auto stats = pool->statistics();
        REQUIRE_EQUAL( stats.releaseCalls, 1U );
        REQUIRE_EQUAL( stats.releaseStored, 1U );
        REQUIRE_EQUAL( stats.releaseEvicted, 0U );
        REQUIRE_EQUAL( stats.freeBufferCount, 1U );
        REQUIRE_EQUAL( stats.freeBufferBytes, 4096U );
        REQUIRE_EQUAL( stats.peakFreeBufferBytes, 4096U );
    }

    /* The best fit is returned, not the first or the largest buffer. */
    {
        ReusableBufferPool<uint8_t> pool;
        pool.setStatisticsEnabled( true );
        pool.release( { std::make_unique<uint8_t[]>( 100 ), 100 } );
        pool.release( { std::make_unique<uint8_t[]>( 200 ), 200 } );
        pool.release( { std::make_unique<uint8_t[]>( 300 ), 300 } );

        auto buffer = pool.acquire( 150 );
        REQUIRE( buffer.data != nullptr );
        REQUIRE_EQUAL( buffer.capacity, 200U );

        const auto stats = pool.statistics();
        REQUIRE_EQUAL( stats.acquireCalls, 1U );
        REQUIRE_EQUAL( stats.acquireHits, 1U );
        REQUIRE_EQUAL( stats.acquireMisses, 0U );
        REQUIRE_EQUAL( stats.reusedBytes, 200U );
        REQUIRE_EQUAL( stats.freeBufferCount, 2U );
        REQUIRE_EQUAL( stats.freeBufferBytes, 400U );
        REQUIRE_EQUAL( stats.peakFreeBufferBytes, 600U );
    }

    /* The free list is bounded and discards the incoming buffer when it is full. */
    {
        ReusableBufferPool<uint8_t> pool;
        pool.setStatisticsEnabled( true );
        for ( size_t i = 0; i < 8; ++i ) {
            pool.release( { std::make_unique<uint8_t[]>( 10 ), 10 } );
        }
        pool.release( { std::make_unique<uint8_t[]>( 20 ), 20 } );

        const auto stats = pool.statistics();
        REQUIRE_EQUAL( stats.releaseCalls, 9U );
        REQUIRE_EQUAL( stats.releaseStored, 9U );
        REQUIRE_EQUAL( stats.releaseEvicted, 1U );
        REQUIRE_EQUAL( stats.freeBufferCount, 8U );
        REQUIRE_EQUAL( stats.freeBufferBytes, 90U );
        REQUIRE_EQUAL( stats.peakFreeBufferBytes, 90U );
    }

    std::cout << "\nTests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
