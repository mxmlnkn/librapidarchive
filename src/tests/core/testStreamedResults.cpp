#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <thread>

#include <core/StreamedResults.hpp>
#include <core/TestHelpers.hpp>


using namespace rapidgzip;


int
main()
{
    StreamedResults<size_t> results;
    std::promise<void> started;
    auto startedFuture = started.get_future();
    std::thread pusher(
        [&] ()
        {
            started.set_value();
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            results.push( 42 );
            results.finalize();
        } );

    startedFuture.wait();
    const auto start = std::chrono::steady_clock::now();
    const auto [value, code] = results.get( 0, -1.0 );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start );
    pusher.join();

    REQUIRE( code == BlockFinderInterface::GetReturnCode::SUCCESS );
    REQUIRE( value.has_value() );
    REQUIRE_EQUAL( *value, size_t( 42 ) );
    REQUIRE( elapsed >= std::chrono::milliseconds( 50 ) );

    /* A zero timeout must not block on a result that is not there yet. */
    StreamedResults<size_t> empty;
    const auto [missing, timeoutCode] = empty.get( 0, 0.0 );
    REQUIRE( !missing );
    REQUIRE( timeoutCode == BlockFinderInterface::GetReturnCode::TIMEOUT );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
