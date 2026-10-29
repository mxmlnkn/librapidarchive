#include <iostream>
#include <cstdint>
#include <limits>
#include <vector>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <core/common.hpp>
#include <core/FileUtils.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <filereader/Standard.hpp>
#include <rapidgzip/IndexFileFormat.hpp>


using namespace rapidgzip;


GzipIndex
testIndexRead( const std::filesystem::path& compressedPath,
               const std::filesystem::path& uncompressedPath,
               const std::filesystem::path& indexPath,
               const size_t                 expectedCheckpointSpacing,
               const size_t                 expectedCheckpointCount )
{
    auto index = readGzipIndex( std::make_unique<StandardFileReader>( indexPath.string() ),
                                /* This second argument is necessary when reading gztool indexes! */
                                std::make_unique<StandardFileReader>( compressedPath ) );

    REQUIRE_EQUAL( index.compressedSizeInBytes, fileSize( compressedPath ) );
    REQUIRE_EQUAL( index.uncompressedSizeInBytes, fileSize( uncompressedPath ) );

    REQUIRE_EQUAL( index.checkpointSpacing, expectedCheckpointSpacing );
    REQUIRE_EQUAL( index.checkpoints.size(), expectedCheckpointCount );

    REQUIRE( static_cast<bool>( index.windows ) );

    return index;
}


void
testIndexReadWrite( const std::filesystem::path& compressedPath,
                    const std::filesystem::path& uncompressedPath,
                    const std::filesystem::path& indexPath,
                    const size_t                 expectedCheckpointSpacing,
                    const size_t                 expectedCheckpointCount )
{
    const auto index = testIndexRead( compressedPath, uncompressedPath, indexPath,
                                      expectedCheckpointSpacing, expectedCheckpointCount );

    try
    {
        const auto tmpFolder = createTemporaryDirectory( "rapidgzip.testGzipIndexFormat" );
        const auto gzipIndexPath = tmpFolder.path() / "gzipindex";

        {
            const auto file = throwingOpen( gzipIndexPath, "wb" );
            const auto checkedWrite =
                [&file] ( const void* buffer, size_t size )
                {
                    if ( std::fwrite( buffer, 1, size, file.get() ) != size ) {
                        throw std::runtime_error( "Failed to write data to index!" );
                    }
                };

            using namespace std::string_literals;
            if ( endsWith( indexPath.string(), ".gztool.index"s )
                 || endsWith( indexPath.string(), ".gztool.with-lines.index"s ) )
            {
                gztool::writeGzipIndex( index, checkedWrite );
            } else {
                indexed_gzip::writeGzipIndex( index, checkedWrite );
            }
        }
        const auto rereadIndex = readGzipIndex( std::make_unique<StandardFileReader>( gzipIndexPath.string() ),
                                                /* This second argument is necessary when reading gztool indexes! */
                                                std::make_unique<StandardFileReader>( compressedPath.string() ) );

        REQUIRE_EQUAL( rereadIndex.compressedSizeInBytes, index.compressedSizeInBytes );
        REQUIRE_EQUAL( rereadIndex.uncompressedSizeInBytes, index.uncompressedSizeInBytes );
        REQUIRE_EQUAL( rereadIndex.checkpointSpacing, index.checkpointSpacing );
        REQUIRE_EQUAL( rereadIndex.windowSizeInBytes, index.windowSizeInBytes );
        REQUIRE_EQUAL( rereadIndex.hasLineOffsets, index.hasLineOffsets );
        REQUIRE_EQUAL( rereadIndex.newlineFormat, index.newlineFormat );

        REQUIRE( rereadIndex.checkpoints == index.checkpoints );

        REQUIRE_EQUAL( static_cast<bool>( rereadIndex.windows ), static_cast<bool>( index.windows ) );
        if ( rereadIndex.windows && index.windows ) {
            REQUIRE_EQUAL( rereadIndex.windows->size(), index.windows->size() );
            const auto& [_, windows] = index.windows->data();
            for ( const auto& [offset, window] : *windows ) {
                const auto rereadWindow = rereadIndex.windows->get( offset );
                if ( !rereadWindow ) {
                    std::cerr << "Failed to find offset " << offset << " in reread index!\n";
                    continue;
                }

                if ( static_cast<bool>( window ) != static_cast<bool>( rereadWindow ) ) {
                    std::stringstream message;
                    message << std::boolalpha << "Shared window has value: " << static_cast<bool>( window )
                            << " while reread shared window has value: " << static_cast<bool>( rereadWindow );
                    std::cerr << std::move( message ).str() << "\n";
                    continue;
                }

                if ( *window != *rereadWindow ) {
                    const auto a = window->decompress();
                    const auto b = rereadWindow->decompress();
                    if ( ( static_cast<bool>( a ) != static_cast<bool>( b ) )
                         || ( static_cast<bool>( a ) && static_cast<bool>( b ) && ( *a != *b ) ) )
                    {
                        std::cerr << "Window contents for offset " << offset << " differ!\n";
                        std::cerr << "Compressed window size: " << window->compressedSize()
                                  << " reread window: " << rereadWindow->compressedSize() << "\n";
                        if ( a && b ) {
                            std::cerr << "Decompressed window size: " << a->size()
                                      << " reread window: " << b->size() << "\n";
                        }
                    }
                }
            }
        }

        REQUIRE( rereadIndex == index );
    }
    catch ( const std::exception& exception )
    {
        /* Note that the destructor for TemporaryDirectory might not be called for uncaught exceptions!
         * @see https://stackoverflow.com/questions/222175/why-destructor-is-not-called-on-exception */
        std::cerr << "Caught exception: " << exception.what() << "\n";
        REQUIRE( false );
    }
}


void
testMalformedIndexSizes( const std::filesystem::path& rootFolder )
{
    const auto compressedPath = rootFolder / "base64-256KiB.gz";
    const auto bgzfPath = rootFolder / "base64-256KiB.bgz";

    const auto requireInvalidArgumentWithoutHugeAllocation =
        [&] ( const std::vector<uint8_t>& indexData,
              const std::filesystem::path&  archivePath,
              const bool                    requireInvalidArgument,
              const std::string&            description )
        {
            bool threw{ false };
            bool threwInvalidArgument{ false };
            bool threwAllocationError{ false };
            try {
                (void)readGzipIndex(
                    std::make_unique<BufferViewFileReader>( indexData ),
                    std::make_unique<StandardFileReader>( archivePath.string() ) );
            } catch ( const std::bad_alloc& ) {
                threwAllocationError = true;
            } catch ( const std::length_error& ) {
                threwAllocationError = true;
            } catch ( const std::invalid_argument& ) {
                threw = true;
                threwInvalidArgument = true;
            } catch ( const std::exception& ) {
                threw = true;
            }

            if ( threwAllocationError ) {
                std::cerr << description << " requested a huge allocation instead of failing at EOF!\n";
            }
            if ( requireInvalidArgument && !threwInvalidArgument ) {
                std::cerr << description << " did not throw std::invalid_argument!\n";
            }
            REQUIRE( threw );
            REQUIRE( !threwAllocationError );
            REQUIRE( !requireInvalidArgument || threwInvalidArgument );
        };

    /* indexed_gzip: 4-byte little-endian checkpoint count at offset 31. */
    {
        auto indexData = readFile<std::vector<uint8_t> >( rootFolder / "base64-256KiB.gz.index" );
        REQUIRE( indexData.size() > 35 );
        constexpr uint32_t HUGE_CHECKPOINT_COUNT = 0xFFFFFFFFU;
        for ( size_t i = 0; i < 4; ++i ) {
            indexData[31 + i] = static_cast<uint8_t>( ( HUGE_CHECKPOINT_COUNT >> ( 8 * i ) ) & 0xFFU );
        }
        requireInvalidArgumentWithoutHugeAllocation( indexData, compressedPath, false,
                                                     "indexed_gzip checkpoint count" );
    }

    /* gztool: big-endian 64-bit counts after the 16-byte header and the optional 4-byte newline format.
     * Each checkpoint record is 24 B header + optional compressed window. */
    {
        auto indexData = readFile<std::vector<uint8_t> >( rootFolder / "base64-256KiB.gz.gztool.with-lines.index" );
        REQUIRE( indexData.size() > 60 );
        REQUIRE_EQUAL( indexData[15], static_cast<uint8_t>( 'X' ) );
        constexpr size_t COUNT_OFFSET = 20;
        auto hugeCountData = indexData;
        for ( size_t i = 0; i < 8; ++i ) {
            hugeCountData[COUNT_OFFSET + i] = 0xFF;
            hugeCountData[COUNT_OFFSET + 8 + i] = 0xFF;
        }
        requireInvalidArgumentWithoutHugeAllocation( hugeCountData, compressedPath, false, "gztool checkpoint count" );

        /* The first record starts after both counts. Its compressed window size is the last 4 bytes. */
        auto hugeWindowData = indexData;
        constexpr size_t FIRST_WINDOW_SIZE_OFFSET = COUNT_OFFSET + 16 + 8 + 8 + 4;
        for ( size_t i = 0; i < 4; ++i ) {
            hugeWindowData[FIRST_WINDOW_SIZE_OFFSET + i] = 0xFF;
        }
        requireInvalidArgumentWithoutHugeAllocation( hugeWindowData, compressedPath, true,
                                                     "gztool compressed window size" );
    }

    /* bgzip .gzi: a little-endian uint64 count followed by (compressed, uncompressed) pairs.
     * Choose a count that wraps (2 * count + 1) * 8 to the real file size to exercise the old
     * file-size bypass before the huge allocation. */
    {
        auto indexData = readFile<std::vector<uint8_t> >( rootFolder / "base64-256KiB.bgz.gzi" );
        REQUIRE( indexData.size() % ( 2 * sizeof( uint64_t ) ) == sizeof( uint64_t ) );
        const auto numberOfValues = indexData.size() / sizeof( uint64_t );
        const auto wrappedNumberOfEntries = ( numberOfValues - 1 ) / 2;
        const auto hugeCount = ( 1ULL << 60U ) + wrappedNumberOfEntries;
        for ( size_t i = 0; i < 8; ++i ) {
            indexData[i] = static_cast<uint8_t>( ( hugeCount >> ( 8 * i ) ) & 0xFFU );
        }
        requireInvalidArgumentWithoutHugeAllocation( indexData, bgzfPath, false, "bgzip checkpoint count" );

        /* A raw checkpoint offset that overflows when adding the gzip header and converting to bits. */
        auto offsetData = readFile<std::vector<uint8_t> >( rootFolder / "base64-256KiB.bgz.gzi" );
        constexpr uint64_t HUGE_OFFSET = ( 1ULL << 61U ) - 2U;
        for ( size_t i = 0; i < 8; ++i ) {
            offsetData[8 + i] = static_cast<uint8_t>( ( HUGE_OFFSET >> ( 8 * i ) ) & 0xFFU );
        }
        requireInvalidArgumentWithoutHugeAllocation( offsetData, bgzfPath, true, "bgzip checkpoint offset overflow" );
    }
}


void
testCraftedIndexedGzipIndexes()
{
    const auto appendLittleEndian = [] ( std::vector<uint8_t>& data, auto value ) {
        for ( size_t i = 0; i < sizeof( value ); ++i ) {
            data.push_back( static_cast<uint8_t>( ( value >> ( 8 * i ) ) & 0xFFU ) );
        }
    };

    const auto makeHeader = [&] ( const uint64_t compressedSize,
                                  const uint64_t uncompressedSize,
                                  const uint32_t checkpointCount )
    {
        std::vector<uint8_t> data{ 'G', 'Z', 'I', 'D', 'X', 1, 0 };
        appendLittleEndian( data, compressedSize );
        appendLittleEndian( data, uncompressedSize );
        appendLittleEndian( data, uint32_t( 1 ) );       // checkpoint spacing
        appendLittleEndian( data, uint32_t( 32_Ki ) );   // window size
        appendLittleEndian( data, checkpointCount );
        return data;
    };

    const auto requireInvalidArgument = [&] ( const std::vector<uint8_t>& indexData,
                                              const std::string&            description )
    {
        bool threwInvalidArgument{ false };
        bool threwAllocationError{ false };
        try {
            (void)readGzipIndex( std::make_unique<BufferViewFileReader>( indexData ) );
        } catch ( const std::bad_alloc& ) {
            threwAllocationError = true;
        } catch ( const std::length_error& ) {
            threwAllocationError = true;
        } catch ( const std::invalid_argument& ) {
            threwInvalidArgument = true;
        } catch ( const std::exception& ) {
        }

        if ( !threwInvalidArgument || threwAllocationError ) {
            std::cerr << description << " did not fail with std::invalid_argument without huge allocation!\n";
        }
        REQUIRE( threwInvalidArgument );
        REQUIRE( !threwAllocationError );
    };

    /* A compressed offset that overflows when converted to bits must be rejected. */
    {
        constexpr uint64_t MAX_OFFSET = std::numeric_limits<uint64_t>::max() / 8U;
        auto indexData = makeHeader( std::numeric_limits<uint64_t>::max(), 1, 1 );
        appendLittleEndian( indexData, MAX_OFFSET + 1U );
        appendLittleEndian( indexData, uint64_t( 0 ) );
        indexData.push_back( 0 );  // bit offset
        indexData.push_back( 0 );  // no window data
        requireInvalidArgument( indexData, "indexed_gzip bit-offset overflow" );
    }

    /* Many checkpoints with the data flag set must not allocate window memory before the index runs out. */
    {
        constexpr uint32_t CHECKPOINT_COUNT = 4096;
        auto indexData = makeHeader( CHECKPOINT_COUNT + 1U, CHECKPOINT_COUNT, CHECKPOINT_COUNT );
        for ( uint32_t i = 0; i < CHECKPOINT_COUNT; ++i ) {
            appendLittleEndian( indexData, uint64_t( i ) );  // compressed offset
            appendLittleEndian( indexData, uint64_t( i ) );  // uncompressed offset
            indexData.push_back( 0 );                       // bit offset
            indexData.push_back( 1 );                       // data flag: window present
        }
        requireInvalidArgument( indexData, "indexed_gzip window amplification" );
    }
}


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
    const auto rootFolder =
        static_cast<std::filesystem::path>(
            findParentFolderContaining( binaryFolder, "src/tests/data/base64-256KiB.bgz" )
        ) / "src" / "tests" / "data";

    using namespace std::string_literals;
    testIndexReadWrite( rootFolder / "base64-256KiB.gz",
                        rootFolder / "base64-256KiB",
                        rootFolder / "base64-256KiB.gz.index",
                        64_Ki, 5U );

    testIndexReadWrite( rootFolder / "base64-256KiB.gz",
                        rootFolder / "base64-256KiB",
                        rootFolder / "base64-256KiB.gz.gztool.index",
                        0, 5U );

    testIndexReadWrite( rootFolder / "base64-256KiB.gz",
                        rootFolder / "base64-256KiB",
                        rootFolder / "base64-256KiB.gz.gztool.with-lines.index",
                        0, 9U );

    testIndexRead( rootFolder / "base64-256KiB.bgz",
                   rootFolder / "base64-256KiB",
                   rootFolder / "base64-256KiB.bgz.gzi",
                   0, 4U );

    testMalformedIndexSizes( rootFolder );
    testCraftedIndexedGzipIndexes();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
