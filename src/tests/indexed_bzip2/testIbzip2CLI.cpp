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

#if !defined( _WIN32 )
    #include <fcntl.h>
    #include <unistd.h>
#endif

#define WITHOUT_MAIN
#include <ibzip2.cpp>
#undef WITHOUT_MAIN


using namespace rapidgzip;

namespace
{
#if !defined( _WIN32 )
class ScopedStandardOutputRedirect
{
public:
    explicit
    ScopedStandardOutputRedirect( const std::string& path )
    {
        m_oldFileDescriptor = ::dup( STDOUT_FILENO );
        const auto newFileDescriptor = ::open( path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
        if ( ( m_oldFileDescriptor < 0 ) || ( newFileDescriptor < 0 )
             || ( ::dup2( newFileDescriptor, STDOUT_FILENO ) < 0 ) ) {
            if ( newFileDescriptor >= 0 ) {
                ::close( newFileDescriptor );
            }
            throw std::runtime_error( "Could not redirect stdout to '" + path + "'." );
        }
        ::close( newFileDescriptor );
    }

    ~ScopedStandardOutputRedirect()
    {
        if ( m_oldFileDescriptor >= 0 ) {
            ::dup2( m_oldFileDescriptor, STDOUT_FILENO );
            ::close( m_oldFileDescriptor );
        }
    }

    ScopedStandardOutputRedirect( const ScopedStandardOutputRedirect& ) = delete;
    ScopedStandardOutputRedirect& operator=( const ScopedStandardOutputRedirect& ) = delete;

private:
    int m_oldFileDescriptor{ -1 };
};
#endif


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


std::string
readTextFile( const std::string& path )
{
    const auto bytes = readFile<std::vector<char> >( path );
    return std::string( bytes.begin(), bytes.end() );
}


void
testStreamingOffsets( const std::filesystem::path& dataFolder )
{
    const auto compressedPath = ( dataFolder / "base64-256KiB.bz2" ).string();
    const auto data = readFile<std::vector<char> >( dataFolder / "base64-256KiB" );
    const auto tmpFolder = createTemporaryDirectory( "indexed_bzip2.testIbzip2CLI" );
    const auto listPath = ( tmpFolder.path() / "listed.index" ).string();
    const auto exportPath = ( tmpFolder.path() / "exported.index" ).string();
    const auto decodedPath = ( tmpFolder.path() / "decoded" ).string();

    /* -L is an alias for --export-index. Decode while writing the streamed index to a file. */
    callCLI( "./ibzip2", ibzip2CLI, { "-f", "-L", listPath, "-d", "-o", decodedPath, compressedPath } );
    REQUIRE( readFile<std::vector<char> >( decodedPath ) == data );

    /* A standalone export uses the same code path and must produce the same bytes. */
    callCLI( "./ibzip2", ibzip2CLI, { "-f", "--export-index", exportPath, compressedPath } );
    REQUIRE_EQUAL( readTextFile( listPath ), readTextFile( exportPath ) );

    /* The former --list-offsets spelling remains available as an alias. */
    callCLI( "./ibzip2", ibzip2CLI, { "-f", "--list-offsets", listPath, compressedPath } );
    REQUIRE_EQUAL( readTextFile( listPath ), readTextFile( exportPath ) );

#if !defined( _WIN32 )
    /* A dash writes the index to stdout when the decoded data goes to a file. */
    const auto stdoutPath = ( tmpFolder.path() / "stdout.index" ).string();
    {
        const ScopedStandardOutputRedirect redirect( stdoutPath );
        callCLI( "./ibzip2", ibzip2CLI,
                 { "-f", "--export-index", "-", "-d", "-o", decodedPath, compressedPath } );
    }
    REQUIRE( readFile<std::vector<char> >( decodedPath ) == data );
    REQUIRE_EQUAL( readTextFile( stdoutPath ), readTextFile( listPath ) );
#endif
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
    testStreamingOffsets( dataFolder );
    testTestOption( dataFolder );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";
    return gnTestErrors == 0 ? 0 : 1;
}
