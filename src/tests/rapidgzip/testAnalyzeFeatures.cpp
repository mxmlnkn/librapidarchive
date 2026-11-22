#include <iostream>
#include <string>
#include <string_view>

#include <core/TestHelpers.hpp>
#include <rapidgzip/gzip/AnalyzeFeatures.hpp>


using namespace rapidgzip;
using namespace rapidgzip::deflate;


void
testEmptySpecificationSelectsEverything()
{
    const auto features = parseAnalyzeFeatures( "" );
    REQUIRE( features == AnalyzeFeatureSet::all() );
    REQUIRE( features.contains( AnalyzeFeature::BLOCK_INFO ) );
    REQUIRE( features.contains( AnalyzeFeature::HUFFMAN_INFO ) );
    REQUIRE( features.contains( AnalyzeFeature::BLOCK_STATS ) );
    REQUIRE( features.contains( AnalyzeFeature::HUFFMAN_STATS ) );
    REQUIRE( features.contains( AnalyzeFeature::STREAM_INFO ) );
    REQUIRE( features.contains( AnalyzeFeature::PROFILE ) );
}


void
testBareListSelectsExactly()
{
    const auto features = parseAnalyzeFeatures( "block-stats,huffman-stats" );
    REQUIRE( features == ( [] {
                               AnalyzeFeatureSet result;
                               result.set( AnalyzeFeature::BLOCK_STATS );
                               result.set( AnalyzeFeature::HUFFMAN_STATS );
                               return result;
                           }() ) );
    REQUIRE( !features.contains( AnalyzeFeature::BLOCK_INFO ) );
    REQUIRE( !features.contains( AnalyzeFeature::HUFFMAN_INFO ) );
    REQUIRE( !features.contains( AnalyzeFeature::STREAM_INFO ) );
    REQUIRE( !features.contains( AnalyzeFeature::PROFILE ) );
}


void
testPrefixesModifyDefault()
{
    {
        const auto features = parseAnalyzeFeatures( "-block-info" );
        REQUIRE( features == ( [] {
                                   auto result = AnalyzeFeatureSet::all();
                                   result.set( AnalyzeFeature::BLOCK_INFO, false );
                                   return result;
                               }() ) );
        REQUIRE( features.contains( AnalyzeFeature::HUFFMAN_INFO ) );
    }

    {
        const auto features = parseAnalyzeFeatures( "+huffman-stats,-block-info" );
        REQUIRE( features.contains( AnalyzeFeature::HUFFMAN_STATS ) );
        REQUIRE( !features.contains( AnalyzeFeature::BLOCK_INFO ) );
        REQUIRE( features.contains( AnalyzeFeature::BLOCK_STATS ) );
        REQUIRE( features.contains( AnalyzeFeature::STREAM_INFO ) );
    }

    {
        const auto features = parseAnalyzeFeatures( "+block-info" );
        REQUIRE( features == AnalyzeFeatureSet::all() );
    }
}


void
testWhitespaceAndAlias()
{
    const auto features = parseAnalyzeFeatures( " block-details , huffman-info " );
    REQUIRE( features.contains( AnalyzeFeature::BLOCK_INFO ) );
    REQUIRE( features.contains( AnalyzeFeature::HUFFMAN_INFO ) );
    REQUIRE( !features.contains( AnalyzeFeature::BLOCK_STATS ) );

    const auto prefixed = parseAnalyzeFeatures( " +huffman-info , -block-info " );
    REQUIRE( prefixed.contains( AnalyzeFeature::HUFFMAN_INFO ) );
    REQUIRE( !prefixed.contains( AnalyzeFeature::BLOCK_INFO ) );
    REQUIRE( prefixed.contains( AnalyzeFeature::BLOCK_STATS ) );
}


void
testInvalidSpecificationsThrow()
{
    REQUIRE_THROWS( parseAnalyzeFeatures( "unknown" ) );
    REQUIRE_THROWS( parseAnalyzeFeatures( "block-info,,block-stats" ) );
    REQUIRE_THROWS( parseAnalyzeFeatures( "+block-info,block-stats" ) );
    REQUIRE_THROWS( parseAnalyzeFeatures( "+" ) );
}


int
main()
{
    testEmptySpecificationSelectsEverything();
    testBareListSelectsExactly();
    testPrefixesModifyDefault();
    testWhitespaceAndAlias();
    testInvalidSpecificationsThrow();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
