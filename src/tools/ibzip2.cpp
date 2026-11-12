#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cxxopts.hpp>

#include <core/AffinityHelpers.hpp>
#include <core/BitStringFinder.hpp>
#include <core/common.hpp>
#include <core/FileRanges.hpp>
#include <core/FileUtils.hpp>
#include <core/ParallelBitStringFinder.hpp>
#include <filereader/Standard.hpp>
#include <indexed_bzip2/bzip2.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/Bzip2Compressor.hpp>
#include <indexed_bzip2/IndexFileFormat.hpp>
#include <indexed_bzip2/ParallelBZ2Reader.hpp>

#include "CLIHelper.hpp"
#include "thirdparty.hpp"


using namespace rapidgzip;
using namespace indexed_bzip2;


namespace
{
/* Check whether the found offsets actually point to BZ2 magic bytes. */
void
checkOffsets( const std::string&         filePath,
              const std::vector<size_t>& offsets )
{
    if ( !fileExists( filePath ) ) {
        return;
    }

    const std::set<uint64_t> bitStringsToFind = { bzip2::MAGIC_BITS_BLOCK, bzip2::MAGIC_BITS_EOS };
    bzip2::BitReader bitReader( std::make_unique<StandardFileReader>( filePath ) );
    for ( const auto offset : offsets ) {
        bitReader.seek( static_cast<long long int>( offset ) );
        /* Because bitReader is limited to 32-bit. */
        static_assert( bzip2::MAGIC_BITS_SIZE % 2 == 0, "Assuming magic bit string size to be an even number." );
        constexpr uint8_t BITS_PER_READ = bzip2::MAGIC_BITS_SIZE / 2;
        const auto magicBytes = ( static_cast<uint64_t>( bitReader.read( BITS_PER_READ ) ) << BITS_PER_READ )
                                | bitReader.read( BITS_PER_READ );

        if ( bitStringsToFind.count( magicBytes ) == 0 ) {
            std::stringstream msg;
            msg << "Magic bytes " << std::hex << magicBytes << std::dec << " at offset "
                << ( offset / CHAR_BIT ) << " B " << ( offset % CHAR_BIT ) << "b "
                << "do not match bzip2 magic bytes!";
            throw std::logic_error( std::move( msg ).str() );
        }
    }
}


void
dumpOffsets( const std::vector<size_t>&  offsets,
             const CheckedWrite&         checkedWrite )
{
    for ( const auto offset : offsets ) {
        const auto line = std::to_string( offset ) + "\n";
        checkedWrite( line.data(), line.size() );
    }
}


void
findCompressedBlocks( const std::string& inputFilePath,
                      const std::string& offsetOutputFilePath,
                      const unsigned int parallelism,
                      const unsigned int bufferSize,
                      const bool         test,
                      const bool         verbose )
{
    /* Having to go over the file twice is not optimal. This is because, the bit string finder is highly
     * optimized for finding non-EOS blocks for parallel decoding. Allowing a list of bit strings to test
     * for would make that algorithm more complex and possibly slower. It was not intended for debugging output */
    std::vector<size_t> offsets;
    const std::set<uint64_t> bitStringsToFind = { bzip2::MAGIC_BITS_BLOCK, bzip2::MAGIC_BITS_EOS };
    for ( const auto bitStringToFind : bitStringsToFind ) {
        using Finder = BitStringFinder<bzip2::MAGIC_BITS_SIZE>;
        using ParallelFinder = ParallelBitStringFinder<bzip2::MAGIC_BITS_SIZE>;

        auto file = openFileOrStdin( inputFilePath );

        std::unique_ptr<Finder> finder =
            parallelism == 1
            ? std::make_unique<Finder>( std::move( file ), bitStringToFind, bufferSize )
            : std::make_unique<ParallelFinder>( std::move( file ), bitStringToFind, parallelism, 0, bufferSize );

        for ( auto offset = finder->find(); offset != std::numeric_limits<size_t>::max(); offset = finder->find() ) {
            offsets.push_back( offset );
        }
    }

    std::sort( offsets.begin(), offsets.end() );

    if ( test ) {
        checkOffsets( inputFilePath, offsets );
    }

    OutputFile output( offsetOutputFilePath );
    dumpOffsets( offsets, [&output] ( const void* data, const size_t size ) { output.write( data, size ); } );
    output.truncateToWrittenSize();

    if ( verbose ) {
        std::cout << "Found " << offsets.size() << " blocks\n";
    }
}

void
printIbzip2Help( const cxxopts::Options& options )
{
    std::cout
    << options.help()
    << "\n"
    << "If no file names are given, ibzip2 decompresses from standard input to standard output.\n"
    << "If the output is discarded by piping to /dev/null, then the actual decoding step might\n"
    << "be omitted if neither --test nor -l nor -L nor --force are given.\n"
    << "\n"
    << "Examples:\n"
    << "\n"
    << "Decompress a file:\n"
    << "  ibzip2 -d file.bz2\n"
    << "\n"
    << "Decompress a file in parallel:\n"
    << "  ibzip2 -d -P 0 file.bz2\n"
    << "\n"
    << "Compress a file to bzip2 in parallel:\n"
    << "  ibzip2 -z -P 0 file\n"
    << "  ibzip2 -z -P 0 --level 9 -o file.bz2 file\n"
    << "\n"
    << "Compress and write the block index while compressing, also for stdout:\n"
    << "  ibzip2 -z --level 9 --export-index file.bz2.index -o file.bz2 file\n"
    << "\n"
    << "Write the block offset index while decoding (-L is an alias):\n"
    << "  ibzip2 -L file.bz2.index -- file.bz2\n"
    << "\n"
    << "Find and list the bzip2 block offsets to be used for another tool:\n"
    << "  ibzip2 -l blockoffsets.dat -- file.bz2\n"
    << "\n"
    << "Print the decompressed size or line count:\n"
    << "  ibzip2 --count file.bz2\n"
    << "  ibzip2 --count-lines file.bz2\n"
    << "\n"
    << "Decompress only a byte range:\n"
    << "  ibzip2 -d -c --ranges 10@0,1KiB@15KiB file.bz2\n"
    << "\n"
    << "List block offsets in both the compressed as well as the decompressed data during downloading:\n"
    << "  wget -O- 'ftp://example.com/file.bz2' | tee saved-file.bz2 | ibzip2 -L blockoffsets.dat > /dev/null\n"
    << std::endl;
}


int
ibzip2CLI( int argc, char const* const* argv )
{
    /**
     * @note For some reason implicit values do not mix very well with positional parameters!
     *       Parameters given to arguments with implicit values will be matched by the positional argument instead!
     */
    cxxopts::Options options( "ibzip2",
                              "A parallel bzip2 decompressor and compressor based on the indexed_bzip2 backend from ratarmount" );
    options.add_options( "Decompression" )
    ( "c,stdout", "Output to standard output. This is the default, when reading from standard input." )
    ( "d,decompress", "Force decompression." )
    ( "f,force", "Force overwriting existing output files. "
                 "Also forces decompression even when piped to /dev/null." )
    ( "i,input", "Input file. If none is given, data is read from standard input.",
      cxxopts::value<std::string>() )
    ( "o,output",
      "Output file. If none is given, use the input file name with '.bz2' stripped or '<input file>.out'. "
      "If no input is read from standard input and not output file is given, then will write to standard output.",
      cxxopts::value<std::string>() )
    ( "k,keep", "Keep (do not delete) input file. Only for compatibility. "
                "This tool will not delete anything automatically!" )
    ( "t,test", "Test compressed file integrity." )

    ( "p,block-finder-parallelism",
      "This only has an effect if the parallel decoder is used with the -P option. "
      "If an optional integer >= 1 is given, then that is the number of threads to use for finding bzip2 blocks. "
      "If 0 is given, then the number of physical cores available to this process is used.",
      cxxopts::value<unsigned int>()->default_value( "1" ) )

    ( "P,decoder-parallelism",
      "Use the parallel decoder or compressor. "
      "If an optional integer >= 1 is given, then that is the number of worker threads to use. "
      "Note that there might be further threads being started with non-worker work. "
      "If 0 is given, then the number of physical cores available to this process is used.",
      cxxopts::value<unsigned int>()->default_value( "0" ) );

    options.add_options( "Actions" )
    ( "count", "Print the decompressed size." )
    ( "count-lines", "Print the number of newline characters in the decompressed data." )
    ( "ranges", "Decompress only the specified byte ranges. Example: 10@0,1KiB@15KiB,inf@40.",
      cxxopts::value<std::string>() )
    ( "import-index", "Use an existing bzip2 block offset index in the format written by --export-index.",
      cxxopts::value<std::string>() );

    options.add_options( "Compression" )
    ( "z,compress", "Compress to bzip2. Each worker writes an independent bzip2 stream. "
                    "The complete streams are concatenated in input order, which bzip2 tools accept." )
    ( "level", "bzip2 block size / compression level in the range 1..9 (default 9).",
      cxxopts::value<int>()->default_value( "9" ) );

    options.add_options( "Output" )
    ( "h,help", "Print this help message." )
    ( "q,quiet", "Suppress noncritical error messages." )
    ( "v,verbose", "Be verbose. A second -v (or shorthand -vv) gives even more verbosity." )
    ( "V,version", "Display software version." )
    ( "oss-attributions", "Display open-source software licenses." )
    ( "oss-attributions-yaml", "Display open-source software licenses in YAML format for use with Conda." )
    ( "l,list-compressed-offsets",
      "List only the bzip2 block offsets given in bits one per line to the specified output file. "
      "If no file is given, it will print to stdout or to stderr if the decoded data is already written to stdout. "
      "Specifying '-' as file path, will write to stdout.",
      cxxopts::value<std::string>() )
    ( "L,list-offsets,export-index",
      "Write a bzip2 block offset index with one '<encoded bits>,<decoded bytes>' pair per line. "
      "Entries are written incrementally while decoding or compressing. Specifying '-' writes to stdout, "
      "or to stderr when the output data itself is written to stdout. The former --list-offsets spelling "
      "is accepted as an alias.",
      cxxopts::value<std::string>() );

    options.add_options( "Advanced" )
    ( "buffer-size",
      "Specifies the output buffer size between calls to the Bzip2 decoder and writing to standard out. "
      "If only --export-index is used with nothing else then it affects the input buffer for the block finder.",
      cxxopts::value<unsigned int>()->default_value( "0" ) );

    options.parse_positional( { "input" } );

    /* cxxopts allows to specify arguments multiple times. But if the argument type is not a vector, then only
     * the last value will be kept! Therefore, do not check against this usage and simply use that value.
     * Arguments may only be queried with as if they have (default) values. */

    const auto parsedArgs = options.parse( argc, argv );

    const auto force   = parsedArgs["force"].as<bool>();
    const auto quiet   = parsedArgs["quiet"].as<bool>();
    const auto test    = parsedArgs["test"].as<bool>();
    const auto verbose = parsedArgs["verbose"].as<bool>();

    const auto getParallelism = [] ( const auto p ) { return p > 0 ? p : availablePhysicalCores(); };
    const auto blockFinderParallelism = getParallelism( parsedArgs["block-finder-parallelism"].as<unsigned int>() );
    const auto decoderParallelism     = getParallelism( parsedArgs["decoder-parallelism"].as<unsigned int>() );

    if ( verbose ) {
        for ( auto const* const path : { "input", "output", "list-compressed-offsets", "export-index" } ) {
            std::string value = "<none>";
            try {
                value = getFilePath( parsedArgs, path );
                if ( value.empty() && ( parsedArgs.count( path ) > 0 ) ) {
                    value = "<stdout>";
                }
            } catch ( ... ) {}
            std::cerr << "file path for " << path << ": " << value << "\n";
        }
    }

    /* Check against simple commands like help and version. */

    if ( parsedArgs.count( "help" ) > 0 ) {
        printIbzip2Help( options );
        return 0;
    }

    if ( parsedArgs.count( "version" ) > 0 ) {
        std::cout << "ibzip2, CLI to the indexed and seekable bzip2 decoding library indexed-bzip2 version 1.7.0.\n";
        return 0;
    }

    if ( parsedArgs.count( "oss-attributions" ) > 0 ) {
        std::cout
            << "# " << thirdparty::cxxopts::name << "\n\n"
            << thirdparty::cxxopts::url << "\n\n"
            << thirdparty::cxxopts::license << "\n\n"
            << "# " << thirdparty::bzip2::name << "\n\n"
            << thirdparty::bzip2::url << "\n\n"
            << thirdparty::bzip2::license << "\n";
        return 0;
    }

    if ( parsedArgs.count( "oss-attributions-yaml" ) > 0 ) {
        std::cout
            << "root_name: rapidgzip\n"
            << "third_party_libraries:\n"
            << "  - package_name: " << thirdparty::cxxopts::name << "\n"
            << "    package_version: "
            << static_cast<uint32_t>( cxxopts::version.major ) << "."
            << static_cast<uint32_t>( cxxopts::version.minor ) << "."
            << static_cast<uint32_t>( cxxopts::version.patch ) << "\n"
            << "    license: Unlicense\n"
            << "    licenses:\n"
            << "      - license: Unlicense\n"
            << "        text: " << toYamlString( thirdparty::cxxopts::license ) << "\n"
            << "  - package_name: " << thirdparty::bzip2::name << "\n"
            << "    package_version: 1.0.8\n"
            << "    license: bzip2-1.0.6\n"
            << "    licenses:\n"
            << "      - license: bzip2-1.0.6\n"
            << "        text: " << toYamlString( thirdparty::bzip2::license ) << "\n";
        return 0;
    }

    /* Parse input and output file specifications. */

    if ( parsedArgs.count( "input" ) > 1 ) {
        std::cerr << "One or none bzip2 filename to decompress must be specified!\n";
        return 1;
    }

    std::string inputFilePath;  /* Can be empty. Then, read from STDIN. */
    if ( parsedArgs.count( "input" ) == 1 ) {
        inputFilePath = parsedArgs["input"].as<std::string>();
        if ( inputFilePath == "-" ) {
            inputFilePath.clear();
        }
        if ( !inputFilePath.empty() && std::filesystem::is_directory( inputFilePath ) ) {
            std::cerr << "Input path is a directory: " << inputFilePath << "\n";
            return 1;
        }
    }

    if ( !stdinHasInput() && inputFilePath.empty() ) {
        std::cerr << "Either stdin must have input, e.g., by piping to it, or an input file must be specified!\n";
        return 1;
    }

    const auto compress = parsedArgs.count( "compress" ) > 0;
    const auto writeToStdOut =
        ( parsedArgs.count( "stdout" ) > 0 )
        || ( ( parsedArgs.count( "output" ) > 0 ) && ( parsedArgs["output"].as<std::string>() == "-" ) );
    auto outputFilePath = getFilePath( parsedArgs, "output" );

    /* Parse action arguments. */

    const auto listCompressedOffsets = parsedArgs.count( "list-compressed-offsets" ) > 0;
    const auto exportIndexRequested = parsedArgs.count( "export-index" ) > 0;
    const auto countBytes = parsedArgs.count( "count" ) > 0;
    const auto countLines = parsedArgs.count( "count-lines" ) > 0;
    const auto doTest = test;
    const auto decompress = !compress && ( parsedArgs.count( "decompress" ) > 0 );

    std::optional<std::vector<FileRange> > fileRanges;
    if ( parsedArgs.count( "ranges" ) > 0 ) {
        fileRanges = parseFileRanges( parsedArgs["ranges"].as<std::string>() );
        for ( const auto& range : *fileRanges ) {
            if ( range.offsetIsLine || range.sizeIsLine ) {
                std::cerr << "Line-based ranges are not supported for bzip2. Use byte ranges instead.\n";
                return 1;
            }
        }
    }

    const auto importIndexPath = parsedArgs.count( "import-index" ) > 0
                                 ? parsedArgs["import-index"].as<std::string>() : std::string();
    const auto exportIndexPath = getFilePath( parsedArgs, "export-index" );

    const auto decode = !compress
                        && ( decompress || doTest || countBytes || countLines || fileRanges.has_value()
                             || exportIndexRequested );
    const auto writeOutput = !compress && !doTest && ( decompress || fileRanges.has_value() );

    if ( compress ) {
        if ( !writeToStdOut && outputFilePath.empty() && !inputFilePath.empty() ) {
            outputFilePath = inputFilePath + ".bz2";
        }
    } else if ( writeOutput && !writeToStdOut && outputFilePath.empty() && !inputFilePath.empty() ) {
        const std::string& suffix = ".bz2";
        if ( endsWith( inputFilePath, suffix, /* case sensitive */ false ) ) {
            outputFilePath = std::string( inputFilePath.begin(),
                                          inputFilePath.end()
                                          - static_cast<std::string::difference_type>( suffix.size() ) );
        } else {
            outputFilePath = inputFilePath + ".out";
            if ( !quiet ) {
                std::cerr << "Could not deduce output file name. Will write to '" << outputFilePath << "'\n";
            }
        }
    }

    if ( compress
         && ( ( parsedArgs.count( "decompress" ) > 0 ) || doTest || listCompressedOffsets
              || countBytes || countLines || fileRanges.has_value()
              || !importIndexPath.empty() ) ) {
        std::cerr << "Compression cannot be combined with decompression, test, count, range, or "
                  << "import-index options.\n";
        return 1;
    }
#ifndef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
    if ( compress ) {
        std::cerr << "This build does not have libbz2 compression support enabled!\n";
        return 1;
    }
#endif

    if ( ( writeOutput || compress ) && ( outputFilePath != "/dev/null" )
         && fileExists( outputFilePath ) && !force ) {
        std::cerr << "Output file '" << outputFilePath << "' already exists! Use --force to overwrite.\n";
        return 1;
    }

    if ( !importIndexPath.empty() && !fileExists( importIndexPath ) ) {
        std::cerr << "The block offset index to import was not found: " << importIndexPath << "\n";
        return 1;
    }
    if ( !exportIndexPath.empty() && fileExists( exportIndexPath ) && !force ) {
        std::cerr << "Output file for block offsets '" << exportIndexPath
                  << "' already exists! Use --force to overwrite.\n";
        return 1;
    }

    const auto bufferSize = parsedArgs["buffer-size"].as<unsigned int>();

    const auto compressedOffsetsFilePath = getFilePath( parsedArgs, "list-compressed-offsets" );
    if ( !compressedOffsetsFilePath.empty() && fileExists( compressedOffsetsFilePath ) && !force ) {
        std::cerr << "Output file compressed offsets '" << compressedOffsetsFilePath
                  << "' for offsets already exists! Use --force to overwrite.\n";
        return 1;
    }

    /* Actually do things as requested. */

    if ( compress ) {
        const auto level = parsedArgs["level"].as<int>();
        if ( ( level < 1 ) || ( level > 9 ) ) {
            std::cerr << "bzip2 compression level must be in the range 1..9!\n";
            return 1;
        }

    #ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
        auto inputFile = openFileOrStdin( inputFilePath );
        if ( !inputFile ) {
            std::cerr << "Could not open input file: " << inputFilePath << "!\n";
            return 1;
        }
        auto outputFile = std::make_unique<OutputFile>( outputFilePath );
        const auto outputFileDescriptor = outputFile->fd();

        if ( verbose ) {
            std::cerr << "Compress " << ( inputFilePath.empty() ? "<stdin>" : inputFilePath.c_str() ) << " -> ";
            if ( outputFile->writingToStdout() ) {
                std::cerr << "<stdout>";
            } else {
                std::cerr << outputFilePath;
            }
            std::cerr << " with " << decoderParallelism << " worker(s), level " << level << "\n";
        }

        std::optional<OutputFile> exportIndexFile;
        if ( exportIndexRequested ) {
            exportIndexFile.emplace( exportIndexPath, outputFile->writingToStdout() );
        }
        const auto writeBlockOffset =
            [&exportIndexFile] ( const size_t compressedOffset, const size_t decodedOffset )
            {
                writeBzip2IndexEntry(
                    [&exportIndexFile] ( const void* data, const size_t size )
                    { exportIndexFile->write( data, size ); },
                    compressedOffset, decodedOffset );
            };
        const std::function<void( size_t, size_t )> blockOffsetCallback =
            exportIndexFile ? std::function<void( size_t, size_t )>( writeBlockOffset )
                            : std::function<void( size_t, size_t )>();

        try {
            const auto tCompressionStart = now();
            const auto result = indexed_bzip2::compressBzip2Parallel(
                *inputFile, outputFileDescriptor, level, decoderParallelism, blockOffsetCallback );
            const auto compressionDuration = duration( tCompressionStart );
            if ( exportIndexFile ) {
                exportIndexFile->truncateToWrittenSize();
            }
            outputFile->truncate( result.compressedBytes );
            if ( verbose ) {
                std::cerr << "Compressed " << result.uncompressedBytes << " B to "
                          << result.compressedBytes << " B in " << compressionDuration << " s -> "
                          << static_cast<double>( result.uncompressedBytes ) / 1e6 / compressionDuration
                          << " MB/s input, ratio "
                          << ( result.uncompressedBytes > 0
                               ? static_cast<double>( result.compressedBytes ) / result.uncompressedBytes
                               : 0.0 )
                          << "\n";
            }
        } catch ( const BrokenPipeException& ) {
            return 128 + /* SIGPIPE */ 13;
        } catch ( const std::exception& exception ) {
            std::cerr << "Compression failed: " << exception.what() << "\n";
            return 1;
        }
    #endif
        return 0;
    }

    if ( decode ) {
        if ( verbose ) {
            std::cerr << "Decompress " << inputFilePath << " -> " << outputFilePath << " with " << decoderParallelism
                      << " threads\n";
        }

        auto fileReader = openFileOrStdin( inputFilePath );

        std::unique_ptr<OutputFile> outputFile;
        if ( writeOutput ) {
            outputFile = std::make_unique<OutputFile>( outputFilePath );
        }
        const auto outputFileDescriptor = outputFile ? outputFile->fd() : -1;

        std::unique_ptr<BZ2ReaderInterface> reader;
        if ( decoderParallelism == 1 ) {
            reader = std::make_unique<BZ2Reader>( std::move( fileReader ) );
        } else {
            reader = std::make_unique<ParallelBZ2Reader>( std::move( fileReader ), decoderParallelism );
        }
        if ( !importIndexPath.empty() ) {
            reader->importIndex( std::make_unique<StandardFileReader>( importIndexPath ) );
        }
        reader->setShowProfileOnDestruction( verbose );

        const auto actualBufferSize = bufferSize > 0 ? bufferSize : 1_Mi;
        std::vector<char> buffer( actualBufferSize );
        size_t nBytesDecodedTotal = 0;
        size_t nBytesWrittenTotal = 0;
        size_t newlineCount = 0;

        const auto writeToStdErr = outputFile && outputFile->writingToStdout();
        auto& out = writeToStdErr ? std::cerr : std::cout;

        std::optional<OutputFile> exportIndexFile;
        std::optional<OutputFile> compressedOffsetsFile;
        if ( exportIndexRequested ) {
            exportIndexFile.emplace( exportIndexPath, writeToStdErr );
        }
        if ( listCompressedOffsets ) {
            compressedOffsetsFile.emplace( compressedOffsetsFilePath, writeToStdErr );
        }

        const auto offsetOutputRequested = exportIndexFile.has_value() || compressedOffsetsFile.has_value();
        std::optional<size_t> lastPrintedOffset;
        size_t printedOffsetCount{ 0 };
        const auto printNewOffset =
            [&] ( const size_t compressedOffset,
                  const size_t decodedOffset )
            {
                if ( lastPrintedOffset && ( compressedOffset <= *lastPrintedOffset ) ) {
                    return;
                }
                if ( exportIndexFile ) {
                    writeBzip2IndexEntry(
                        [&exportIndexFile] ( const void* data, const size_t size ) {
                            exportIndexFile->write( data, size );
                        },
                        compressedOffset, decodedOffset );
                }
                if ( compressedOffsetsFile ) {
                    const auto line = std::to_string( compressedOffset ) + "\n";
                    compressedOffsetsFile->write( line.data(), line.size() );
                }
                lastPrintedOffset = compressedOffset;
                ++printedOffsetCount;
            };

        if ( offsetOutputRequested ) {
            reader->setBlockOffsetCallback( printNewOffset );
            /* An imported or already known map will not produce callbacks for its existing entries. */
            for ( const auto [compressedOffset, decodedOffset] : reader->availableBlockOffsets() ) {
                printNewOffset( compressedOffset, decodedOffset );
            }
        }

        std::optional<int> writeError;
        const auto process =
            [&] ( const char* const data,
                  const size_t size )
            {
                nBytesDecodedTotal += size;
                if ( countLines ) {
                    newlineCount += static_cast<size_t>( std::count( data, data + size, '\n' ) );
                }
                if ( !writeOutput ) {
                    return;
                }

                const auto errorCode = writeAllToFd( outputFileDescriptor, data, size );
                if ( errorCode != 0 ) {
                    writeError = errorCode;
                    return;
                }
                nBytesWrittenTotal += size;
            };

        if ( fileRanges ) {
            for ( const auto& range : *fileRanges ) {
                reader->seek( static_cast<long long int>( range.offset ), SEEK_SET );
                auto remaining = range.size;
                while ( remaining > 0 ) {
                    const auto toRead = std::min( buffer.size(), remaining );
                    const auto nBytesRead = reader->read( -1, buffer.data(), toRead );
                    if ( nBytesRead == 0 ) {
                        break;
                    }
                    process( buffer.data(), nBytesRead );
                    if ( writeError ) {
                        break;
                    }
                    remaining -= nBytesRead;
                }
                if ( writeError ) {
                    break;
                }
            }
        } else {
            while ( true ) {
                const auto nBytesRead = reader->read( -1, buffer.data(), buffer.size() );
                if ( nBytesRead == 0 ) {
                    break;
                }
                process( buffer.data(), nBytesRead );
                if ( writeError ) {
                    break;
                }
            }
        }

        if ( writeError ) {
            std::cerr << "Could not write all the decoded data to the specified output: errno " << *writeError << "\n";
            return *writeError == EPIPE ? 128 + /* SIGPIPE */ 13 : 1;
        }

        if ( compressedOffsetsFile ) {
            compressedOffsetsFile->truncateToWrittenSize();
        }
        if ( exportIndexFile ) {
            exportIndexFile->truncateToWrittenSize();
        }

        if ( outputFile ) {
            outputFile->truncate( nBytesWrittenTotal );
            outputFile.reset();  /* Close the file here to include it in the time measurement. */
        }

        if ( verbose ) {
            /* The callback already delivered every offset, so avoid another full block-map copy here.
             * In range mode, blockOffsets() would decode the whole input and could fail on corruption
             * outside the requested range. */
            const auto blockCount = offsetOutputRequested
                                    ? printedOffsetCount
                                    : ( fileRanges
                                        ? reader->availableBlockOffsets().size()
                                        : reader->blockOffsets().size() );
            out << "Found " << blockCount << " blocks\n";
        }

        if ( doTest ) {
            if ( !fileRanges ) {
                const auto readerSize = reader->size();
                if ( !readerSize ) {
                    throw std::logic_error( "Bzip2 reader size should be available at this point!" );
                }
                if ( nBytesDecodedTotal != *readerSize ) {
                    std::stringstream msg;
                    msg << "Wrote less bytes (" << nBytesDecodedTotal << " B) than decoded stream is large("
                        << *readerSize << " B)!";
                    throw std::logic_error( std::move( msg ).str() );
                }
            }
        }

        if ( countBytes || countLines ) {
            if ( countBytes && countLines ) {
                out << nBytesDecodedTotal << " " << newlineCount << "\n";
            } else {
                out << ( countBytes ? nBytesDecodedTotal : newlineCount ) << "\n";
            }
        }

        return 0;
    }

    if ( listCompressedOffsets ) {
        if ( verbose ) {
            std::cerr << "Find block offsets\n";
        }

        /* If effectively only the compressed offsets are requested, then we can use the blockfinder directly! */
        findCompressedBlocks( inputFilePath,
                              compressedOffsetsFilePath,
                              blockFinderParallelism,
                              bufferSize > 0 ? bufferSize : 32_Ki,
                              test,
                              parsedArgs.count( "verbose" ) > 0 );
        return 0;
    }

    std::cerr << "No suitable arguments were given. Please refer to the help!\n\n";

    printIbzip2Help( options );

    return 1;
}
}


#if !defined( WITH_PYTHON_SUPPORT ) && !defined( WITHOUT_MAIN )
int
main( int argc, char** argv )
{
    try {
        return ibzip2CLI( argc, argv );
    } catch ( const EndOfFileReached& exception ) {
        std::cerr << "Unexpected end of file. Truncated or invalid bzip2?\n";
        return 1;
    } catch ( const std::exception& exception ) {
        const std::string_view message{ exception.what() };
        if ( message.empty() ) {
            std::cerr << "Caught exception with typeid: " << typeid( exception ).name() << "\n";
        } else {
            std::cerr << "Caught exception: " << message << "\n";
        }
        return 1;
    }

    return 1;
}
#endif
