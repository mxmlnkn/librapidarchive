#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>

#define WITHOUT_MAIN
#include <ibzip2.cpp>
#undef WITHOUT_MAIN


using namespace rapidgzip;

namespace
{
void
testCount( const std::filesystem::path& dataFolder )
{
    const auto compressedPath = ( dataFolder / "base64-256KiB.bz2" ).string();
    const auto data = readFile<std::vector<char> >( dataFolder / "base64-256KiB" );
    const auto lineCount = std::count( data.begin(), data.end(), '\n' );

    const auto count = callCLI( "./ibzip2", ibzip2CLI, { "--count", compressedPath } );
    REQUIRE_EQUAL( count.out, std::to_string( data.size() ) + "\n" );

    const auto lines = callCLI( "./ibzip2", ibzip2CLI, { "--count-lines", compressedPath } );
    REQUIRE_EQUAL( lines.out, std::to_string( lineCount ) + "\n" );

    const auto both = callCLI( "./ibzip2", ibzip2CLI, { "--count", "--count-lines", compressedPath } );
    REQUIRE_EQUAL( both.out, std::to_string( data.size() ) + " " + std::to_string( lineCount ) + "\n" );
}


void
testRanges( const std::filesystem::path& dataFolder )
{
    const auto compressedPath = ( dataFolder / "base64-256KiB.bz2" ).string();
    const auto data = readFile<std::vector<char> >( dataFolder / "base64-256KiB" );
    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testIbzip2CLI" );
    const auto outputPath = ( tmpFolder.path() / "ranges.out" ).string();

    const std::vector<std::pair<size_t, size_t> > ranges = { { 100, 1000 }, { 1024, 4096 } };
    callCLI( "./ibzip2", ibzip2CLI,
             { "-d", "-f", "-o", outputPath, "--ranges", "100@1000,1024@4096", compressedPath } );

    const auto output = readFile<std::vector<char> >( outputPath );
    std::vector<char> expected;
    for ( const auto& [size, offset] : ranges ) {
        REQUIRE( offset + size <= data.size() );
        expected.insert( expected.end(), data.begin() + static_cast<std::ptrdiff_t>( offset ),
                         data.begin() + static_cast<std::ptrdiff_t>( offset + size ) );
    }
    REQUIRE_EQUAL( output.size(), expected.size() );
    REQUIRE( output == expected );

    const auto lineRange = callCLI( "./ibzip2", ibzip2CLI, { "--ranges", "1L@0", compressedPath },
                                    /* expectSuccess */ false );
    REQUIRE( lineRange.exitCode != 0 );
    REQUIRE( lineRange.err.find( "Line-based ranges are not supported for bzip2" ) != std::string::npos );
}


void
testIndex( const std::filesystem::path& dataFolder )
{
    const auto compressedPath = ( dataFolder / "base64-256KiB.bz2" ).string();
    const auto data = readFile<std::vector<char> >( dataFolder / "base64-256KiB" );
    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testIbzip2CLI" );
    const auto indexPath = ( tmpFolder.path() / "base64-256KiB.index" ).string();
    const auto outputPath = ( tmpFolder.path() / "import.out" ).string();

    callCLI( "./ibzip2", ibzip2CLI, { "-f", "--export-index", indexPath, compressedPath } );
    REQUIRE( std::filesystem::is_regular_file( indexPath ) );
    const auto indexBytes = readFile<std::vector<char> >( indexPath );
    const std::string index( indexBytes.begin(), indexBytes.end() );
    REQUIRE( index.find( ',' ) != std::string::npos );

    callCLI( "./ibzip2", ibzip2CLI, { "-d", "-f", "-o", outputPath, "--import-index", indexPath, compressedPath } );
    REQUIRE( readFile<std::vector<char> >( outputPath ) == data );

    const auto count = callCLI( "./ibzip2", ibzip2CLI, { "--import-index", indexPath, "--count", compressedPath } );
    REQUIRE_EQUAL( count.out, std::to_string( data.size() ) + "\n" );

    const auto missing = callCLI( "./ibzip2", ibzip2CLI,
                                  { "--import-index", ( tmpFolder.path() / "missing.index" ).string(),
                                    "--count", compressedPath }, /* expectSuccess */ false );
    REQUIRE( missing.exitCode != 0 );
    REQUIRE( missing.err.find( "not found" ) != std::string::npos );

    const auto overwrite = callCLI( "./ibzip2", ibzip2CLI, { "--export-index", indexPath, compressedPath },
                                    /* expectSuccess */ false );
    REQUIRE( overwrite.exitCode != 0 );
    REQUIRE( overwrite.err.find( "already exists" ) != std::string::npos );
}


void
testTestOption( const std::filesystem::path& dataFolder )
{
    const auto compressedPath = ( dataFolder / "base64-256KiB.bz2" ).string();
    callCLI( "./ibzip2", ibzip2CLI, { "--test", compressedPath } );
}
}  // namespace


int
main( int    argc,
      char** argv )
{
    if ( argc == 0 ) {
        std::cerr << "Expected at least the launch command as the first argument!\n";
        return 1;
    }

    const std::string binaryFilePath( argv[0] );
    auto binaryFolder = std::filesystem::path( binaryFilePath ).parent_path().string();
    if ( binaryFolder.empty() ) {
        binaryFolder = ".";
    }
    const auto dataFolder =
        std::filesystem::path(
            findParentFolderContaining( binaryFolder, "src/tests/data/base64-256KiB.bz2" )
        ) / "src" / "tests" / "data";

    testCount( dataFolder );
    testRanges( dataFolder );
    testIndex( dataFolder );
    testTestOption( dataFolder );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
