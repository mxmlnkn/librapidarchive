/**
 * Differential test for the packed bzip2 move-to-front update.
 *
 * The SSE2 path shifts 64-byte chunks and masks the chunk that contains the selected position.
 * Positions below 64 use one masked chunk, positions up to 255 shift whole chunks and one masked
 * chunk. Every update must be identical to a plain 256-byte list for all positions and across
 * arbitrary sequences, including the 63/64, 127/128 and 191/192 chunk boundaries.
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>

#include <core/TestHelpers.hpp>
#include <indexed_bzip2/bzip2.hpp>


using namespace rapidgzip;


namespace
{
void
applyReferenceUpdate( std::array<uint8_t, 256>&  list,
                      const size_t               position )
{
    const auto moved = list[position];
    for ( size_t i = position; i > 0; --i ) {
        list[i] = list[i - 1];
    }
    list[0] = moved;
}


void
requireSameList( const bzip2::PackedMTF&         packed,
                 const std::array<uint8_t, 256>& expected )
{
    const auto* const actual = reinterpret_cast<const uint8_t*>( packed.data() );
    for ( size_t i = 0; i < expected.size(); ++i ) {
        REQUIRE_EQUAL( actual[i], expected[i] );
    }
}


void
testChunkBoundaries()
{
    const std::array<size_t, 20> positions = {
        0, 1, 2, 7, 8, 15, 16, 31, 32, 62, 63, 64, 65, 126, 127, 128, 129, 190, 191, 192,
    };

    auto packed = bzip2::createPackedMTF();
    std::array<uint8_t, 256> expected{};
    std::iota( expected.begin(), expected.end(), uint8_t( 0 ) );

    for ( const auto position : positions ) {
        const auto actual = bzip2::moveToFront( packed, position );
        REQUIRE_EQUAL( actual, expected[position] );
        applyReferenceUpdate( expected, position );
        requireSameList( packed, expected );
    }

    for ( size_t position = 193; position < 256; ++position ) {
        const auto actual = bzip2::moveToFront( packed, position );
        REQUIRE_EQUAL( actual, expected[position] );
        applyReferenceUpdate( expected, position );
        requireSameList( packed, expected );
    }
}


void
testRandomSequences()
{
    std::mt19937 random( 0x5A17U );

    for ( size_t sequence = 0; sequence < 40; ++sequence ) {
        auto packed = bzip2::createPackedMTF();
        std::array<uint8_t, 256> expected{};
        std::iota( expected.begin(), expected.end(), uint8_t( 0 ) );

        for ( size_t step = 0; step < 5000; ++step ) {
            const auto position = static_cast<size_t>( random() % 256 );
            const auto actual = bzip2::moveToFront( packed, position );
            REQUIRE_EQUAL( actual, expected[position] );
            applyReferenceUpdate( expected, position );
        }
        requireSameList( packed, expected );
    }
}
}  // namespace


int
main()
{
    testChunkBoundaries();
    testRandomSequences();

    std::cout << "Tests successful: " << ( rapidgzip::gnTests - rapidgzip::gnTestErrors )
              << " / " << rapidgzip::gnTests << "\n";
    return rapidgzip::gnTestErrors == 0 ? 0 : 1;
}
