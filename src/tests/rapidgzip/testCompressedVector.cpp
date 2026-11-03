#include <cstdint>
#include <iostream>
#include <vector>

#include <core/TestHelpers.hpp>
#include <rapidgzip/CompressedVector.hpp>


using namespace rapidgzip;


void
testEmptyCompressedVector()
{
    CompressedVector vector;
    REQUIRE( vector.empty() );
    REQUIRE( vector.decompress()->empty() );
    REQUIRE( vector.compressedData()->empty() );
    REQUIRE_EQUAL( vector.compressedSize(), 0U );
    REQUIRE_EQUAL( vector.decompressedSize(), 0U );

    vector.clear();
    REQUIRE( vector.empty() );
    REQUIRE( vector.decompress()->empty() );
    REQUIRE( vector.compressedData()->empty() );
    REQUIRE_EQUAL( vector.compressedSize(), 0U );
    REQUIRE_EQUAL( vector.decompressedSize(), 0U );
}

void
testCompressedVector( CompressionType compressionType,
                      const std::vector<uint8_t>& toCompress = { 0, 0, 0, 1, 1, 1 } )
{
    CompressedVector<std::vector<uint8_t> > vector( std::vector<uint8_t>( toCompress ), compressionType );
    REQUIRE( !vector.empty() );
    REQUIRE( !vector.compressedData()->empty() );
    REQUIRE_EQUAL( *vector.decompress(), toCompress );
    REQUIRE_EQUAL( vector.decompressedSize(), toCompress.size() );

    vector.clear();
    REQUIRE( vector.empty() );
    REQUIRE( vector.decompress()->empty() );
    REQUIRE( vector.compressedData()->empty() );
    REQUIRE_EQUAL( vector.compressedSize(), 0U );
    REQUIRE_EQUAL( vector.decompressedSize(), 0U );
}


int
main()
{
    testEmptyCompressedVector();
    testCompressedVector( CompressionType::NONE );
    testCompressedVector( CompressionType::GZIP );
    testCompressedVector( CompressionType::ZLIB );

    /* A 32 KiB incompressible buffer exercises the zlib stored-block path and the ISA-L size bound. */
    std::vector<uint8_t> incompressible( 32 * 1024 );
    uint32_t state = 0x1234'5678U;
    for ( auto& byte : incompressible ) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<uint8_t>( state >> 24U );
    }
    testCompressedVector( CompressionType::ZLIB, incompressible );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
