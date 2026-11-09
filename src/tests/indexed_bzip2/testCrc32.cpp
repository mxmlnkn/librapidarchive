/**
 * Differential test for the runtime-dispatched PCLMUL bzip2 CRC.
 *
 * bzip2 uses the non-reflected CRC-32/BZIP2 (polynomial 0x04C11DB7, initial state 0xFFFFFFFF, no final
 * XOR inside the update function, MSB-first bit order). The new path must be bit-identical to the scalar
 * slicing-by-N reference for every length, every initial state, and for chained updates.
 */

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include <core/TestHelpers.hpp>
#include <indexed_bzip2/Bzip2Crc32.hpp>


using namespace rapidgzip;


namespace
{
[[nodiscard]] uint32_t
scalarCRC( uint32_t       crc,
           const uint8_t* data,
           size_t         size )
{
    return bzip2::updateCRC32SliceByN<bzip2::BZIP2_CRC_SLICES>( crc, data, size );
}


void
testKnownCheckValue()
{
    const std::vector<uint8_t> check = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    /* The published CRC-32/BZIP2 check value is the final complemented CRC. */
    REQUIRE_EQUAL( ~bzip2::updateCRC32( 0xFFFFFFFFU, reinterpret_cast<const char*>( check.data() ), check.size() ),
                   0xFC891918U );
    REQUIRE_EQUAL( ~bzip2::updateCRC32( 0xFFFFFFFFU, nullptr, 0 ), 0x00000000U );
}


void
testEveryShortLength()
{
    std::vector<uint8_t> data( 1024 );
    std::mt19937_64 random( 0xC0FFEEULL );
    for ( auto& byte : data ) {
        byte = static_cast<uint8_t>( random() );
    }

    for ( size_t length = 0; length <= data.size(); ++length ) {
        for ( unsigned int trial = 0; trial < 3; ++trial ) {
            const auto initialCRC = static_cast<uint32_t>( random() );
            const auto expected = scalarCRC( initialCRC, data.data(), length );
            const auto actual = bzip2::updateCRC32(
                initialCRC, reinterpret_cast<const char*>( data.data() ), length );
            REQUIRE_EQUAL( actual, expected );
        #if LIBRAPIDARCHIVE_BZIP2_CRC_PCLMUL
            if ( bzip2::hasPCLMULCRC32() ) {
                REQUIRE_EQUAL( bzip2::updateCRC32PCLMUL( initialCRC, data.data(), length ), expected );
            }
        #endif
        #if LIBRAPIDARCHIVE_BZIP2_CRC_VPCLMUL
            if ( bzip2::hasVPCLMULCRC32() ) {
                REQUIRE_EQUAL( bzip2::updateCRC32VPCLMUL( initialCRC, data.data(), length ), expected );
            }
        #endif
        }
    }
}


void
testRandomLongLengths()
{
    std::mt19937_64 random( 0x123456789ABCDEFULL );
    std::vector<uint8_t> data( 1024 * 1024 + 16 );
    for ( auto& byte : data ) {
        byte = static_cast<uint8_t>( random() );
    }

    for ( unsigned int trial = 0; trial < 64; ++trial ) {
        const auto length = static_cast<size_t>( random() % data.size() );
        const auto initialCRC = static_cast<uint32_t>( random() );
        const auto expected = scalarCRC( initialCRC, data.data(), length );

        const auto dispatched = bzip2::updateCRC32(
            initialCRC, reinterpret_cast<const char*>( data.data() ), length );
        REQUIRE_EQUAL( dispatched, expected );
    #if LIBRAPIDARCHIVE_BZIP2_CRC_PCLMUL
        if ( bzip2::hasPCLMULCRC32() ) {
            REQUIRE_EQUAL( bzip2::updateCRC32PCLMUL( initialCRC, data.data(), length ), expected );

            /* Chained calls must preserve the raw state across arbitrary chunk boundaries. */
            const auto split = static_cast<size_t>( random() % ( length + 1 ) );
            auto chained = bzip2::updateCRC32PCLMUL( initialCRC, data.data(), split );
            chained = bzip2::updateCRC32PCLMUL( chained, data.data() + split, length - split );
            REQUIRE_EQUAL( chained, expected );
        }
    #endif
    #if LIBRAPIDARCHIVE_BZIP2_CRC_VPCLMUL
        if ( bzip2::hasVPCLMULCRC32() ) {
            REQUIRE_EQUAL( bzip2::updateCRC32VPCLMUL( initialCRC, data.data(), length ), expected );

            const auto split = static_cast<size_t>( random() % ( length + 1 ) );
            auto chained = bzip2::updateCRC32VPCLMUL( initialCRC, data.data(), split );
            chained = bzip2::updateCRC32VPCLMUL( chained, data.data() + split, length - split );
            REQUIRE_EQUAL( chained, expected );
        }
    #endif
    }
}


void
testTailAcrossBlockBoundary()
{
    /* Sizes around the 16-byte fold granularity and the fold-by-4 path exercise every tail length. */
    std::mt19937_64 random( 0xFEEDFACEULL );
    std::vector<uint8_t> data( 4096 );
    for ( auto& byte : data ) {
        byte = static_cast<uint8_t>( random() );
    }

    for ( size_t length = 0; length <= data.size(); ++length ) {
        const auto initialCRC = static_cast<uint32_t>( random() );
        const auto expected = scalarCRC( initialCRC, data.data(), length );
    #if LIBRAPIDARCHIVE_BZIP2_CRC_PCLMUL
        if ( bzip2::hasPCLMULCRC32() ) {
            REQUIRE_EQUAL( bzip2::updateCRC32PCLMUL( initialCRC, data.data(), length ), expected );
        }
    #endif
    #if LIBRAPIDARCHIVE_BZIP2_CRC_VPCLMUL
        if ( bzip2::hasVPCLMULCRC32() ) {
            REQUIRE_EQUAL( bzip2::updateCRC32VPCLMUL( initialCRC, data.data(), length ), expected );
        }
    #endif
    }
}
}  // namespace


int
main()
{
    testKnownCheckValue();
    testEveryShortLength();
    testRandomLongLengths();
    testTailAcrossBlockBoundary();

    std::cout << "Tests successful: " << ( rapidgzip::gnTests - rapidgzip::gnTestErrors )
              << " / " << rapidgzip::gnTests << "\n";
    return rapidgzip::gnTestErrors == 0 ? 0 : 1;
}
