#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <exception>
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
#include <filereader/Shared.hpp>
#include <indexed_bzip2/bzip2.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/Bzip2Compressor.hpp>
#include <indexed_bzip2/IndexFileFormat.hpp>
#include <indexed_bzip2/ParallelBZ2Reader.hpp>

#include "CLIHelper.hpp"
#include "thirdparty.hpp"


namespace indexed_bzip2
{
using namespace rapidgzip;


void
dumpOffsets( const std::vector<uint64_t>& offsets,
             const CheckedWrite&          checkedWrite )
{
    for ( const auto offset : offsets ) {
        const auto line = std::to_string( offset ) + "\n";
        checkedWrite( line.data(), line.size() );
    }
}


[[nodiscard]] std::vector<uint64_t>
findCompressedBlocks( const FileReader&  fileReader,
                      const unsigned int parallelism,
                      const unsigned int bufferSize )
{
    /* Having to go over the file twice is not optimal. This is because, the bit string finder is highly
     * optimized for finding non-EOS blocks for parallel decoding. Allowing a list of bit strings to test
     * for would make that algorithm more complex and possibly slower. It was not intended for debugging output */
    std::vector<uint64_t> offsets;
    const std::set<uint64_t> bitStringsToFind = { bzip2::MAGIC_BITS_BLOCK, bzip2::MAGIC_BITS_EOS };
    for ( const auto bitStringToFind : bitStringsToFind ) {
        using Finder = BitStringFinder<bzip2::MAGIC_BITS_SIZE>;
        using ParallelFinder = ParallelBitStringFinder<bzip2::MAGIC_BITS_SIZE>;

        /* Clone the already-open reader instead of reopening the path. The clone shares the underlying file
         * and uses pread, so it also works inside a Landlock sandbox that denies new path opens. */
        auto file = fileReader.clone();

        std::unique_ptr<Finder> finder =
            parallelism == 1
            ? std::make_unique<Finder>( std::move( file ), bitStringToFind, bufferSize )
            : std::make_unique<ParallelFinder>( std::move( file ), bitStringToFind, parallelism, 0, bufferSize );

        for ( auto offset = finder->find(); offset; offset = finder->find() ) {
            offsets.push_back( *offset );
        }
    }

    std::sort( offsets.begin(), offsets.end() );
    return offsets;
}

void
printIbzip2Help( const cxxopts::Options& options )
{
    std::cout
    << options.help()
    << "\n"
    << "If no file names are given, ibzip2 decompresses from standard input to standard output.\n"
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
    cxxopts::Options options(
        "ibzip2", "A parallel bzip2 decompressor and compressor based on the indexed_bzip2 backend from ratarmount" );

    addCommonFileOptions( options, ".bz2", /* countLinesShort */ "" );

    options.add_options( "Output" )
    ( "l,list-compressed-offsets",
      "List only the bzip2 block offsets given in bits one per line to the specified output file. "
      "If no file is given, it will print to stdout or to stderr if the decoded data is already written to stdout. "
      "Specifying '-' as file path, will write to stdout.",
      cxxopts::value<std::string>() );
    addExportIndexOption( options, "L,list-offsets",
                          "Write a block offset index with one '<encoded bits>,<decoded bytes>' pair "
                          "per line. Entries are written incrementally while decoding or compressing. "
                          "Specifying '-' writes to stdout, or to stderr when the output data itself is written "
                          "to stdout. The former --list-offsets spelling is accepted as an alias." );

    options.add_options( "Decompression" )
    ( "p,block-finder-parallelism",
      "This only has an effect if the parallel decoder is used with the -P option. "
      "If an optional integer >= 1 is given, then that is the number of threads to use for finding bzip2 blocks. "
      "If 0 is given, the finder parallelism is chosen automatically from the decoder parallelism (1 to 8).",
      cxxopts::value<unsigned int>()->default_value( "1" ) );

    options.add_options( "Compression" )
    ( "z,compress", "Compress to bzip2. Each worker writes an independent bzip2 stream. "
                    "The complete streams are concatenated in input order, which bzip2 tools accept." )
    ( "level", "bzip2 block size / compression level in the range 1..9 (default 9).",
      cxxopts::value<int>()->default_value( "9" ) );

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

    /* Check against simple commands like help and version. */

    const auto informationalExitCode = handleInformationalAction(
        parsedArgs, "indexed_bzip2",
        "ibzip2, CLI to the indexed and seekable bzip2 decoding library indexed-bzip2 version 2.0.1\n",
        std::array{
            thirdparty::CXXOPTS,
        #ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
            thirdparty::BZIP2,
        #endif
            thirdparty::LIBSAIS,
            thirdparty::TOYBOX,
        },
        [&options] () { printIbzip2Help( options ); } );
    if ( informationalExitCode ) {
        return *informationalExitCode;
    }

    const auto compress = parsedArgs.count( "compress" ) > 0;
#ifndef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
    if ( compress ) {
        std::cerr << "This build does not have libbz2 compression support enabled!\n";
        return 1;
    }
#endif

    const auto level = parsedArgs["level"].as<int>();
    if ( compress && ( ( level < 1 ) || ( level > 9 ) ) ) {
        std::cerr << "bzip2 compression level must be in the range 1..9!\n";
        return 1;
    }

    const auto compressedOffsetsFilePath = getFilePath( parsedArgs, "list-compressed-offsets" );
    if ( !checkOutputFileWritable( compressedOffsetsFilePath, parsedArgs["force"].as<bool>() ) ) {
        return 1;
    }

    const auto listCompressedOffsets = parsedArgs.count( "list-compressed-offsets" ) > 0;
    const auto blockFinderParallelism = parsedArgs["block-finder-parallelism"].as<unsigned int>();
    const auto bufferSize = parsedArgs["buffer-size"].as<unsigned int>();

    /* Keep the line-range limitation local to ibzip2 so it is easy to remove later. Validate before
     * parseArguments opens any deduced output file. */
    if ( parsedArgs.count( "ranges" ) > 0 ) {
        for ( const auto& range : parseFileRanges( parsedArgs["ranges"].as<std::string>() ) ) {
            if ( range.offsetIsLine || range.sizeIsLine ) {
                std::cerr << "Line-based ranges are not supported for bzip2. Use byte ranges instead.\n";
                return 1;
            }
        }
    }

    std::optional<OutputFile> compressedOffsetsFile;
    ArgumentsOptions argumentsOptions;
    argumentsOptions.compressionSuffix = ".bz2";
    argumentsOptions.decompressionSuffixes = { ".bz2", ".bzip2" };
    argumentsOptions.additionalPathOptions = { "list-compressed-offsets" };
    if ( listCompressedOffsets ) {
        argumentsOptions.openAdditionalFiles =
            [&compressedOffsetsFile, &compressedOffsetsFilePath] ( Arguments& args )
            {
                compressedOffsetsFile.emplace( compressedOffsetsFilePath,
                                               /* useStderr */ args.primaryOutputIsStdout() );
                return true;
            };
    }

    Arguments args;
    if ( !parseArguments( args, parsedArgs, argumentsOptions ) ) {
        return 1;
    }
    if ( args.decoderParallelism == 0 ) {
        args.decoderParallelism = availablePhysicalCores();
    }

    /* Parse input and output file specifications. */

    if ( ( parsedArgs.count( "decompress" ) > 0 ) && compress ) {
        std::cerr << "Only either compression or decompression may be requested!\n";
        return 1;
    }

    const auto decode = !compress
                        && ( args.decompress || args.test || args.countBytes || args.countLines
                             || args.fileRanges || args.indexSaveFile );
    /* --test only implies discarding the result when no explicit output was requested. With -c or -o, the
     * data is still written while it is verified, matching rapidgzip and the usual CLI expectation.
     * Do not special-case /dev/null: the write path is intentionally kept identical to any other output so
     * that CLI benchmark results remain comparable with other decoders. */
    const auto writeOutput = !compress && args.writesOutput();

    /* Actually do things as requested. */

    if ( compress ) {
    #ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
        auto* const outputFile = args.primaryOutputFile();
        const auto outputFileDescriptor = outputFile->fd();

        if ( args.verbose ) {
            std::cerr << "Compress " << ( args.inputFilePath.empty() ? "<stdin>" : args.inputFilePath.c_str() )
                      << " -> ";
            if ( outputFile->writingToStdout() ) {
                std::cerr << "<stdout>";
            } else {
                std::cerr << args.outputFilePath;
            }
            std::cerr << " with " << args.decoderParallelism << " worker(s), level " << level << "\n";
        }

        const auto writeBlockOffset =
            [&args] ( const uint64_t compressedOffset, const uint64_t decodedOffset )
            {
                writeBzip2IndexEntry(
                    [&args] ( const void* data, const size_t size )
                    { args.indexSaveFile->write( data, size ); },
                    compressedOffset, decodedOffset );
            };
        const std::function<void( uint64_t, uint64_t )> blockOffsetCallback =
            args.indexSaveFile
            ? std::function<void( uint64_t, uint64_t )>( writeBlockOffset )
            : std::function<void( uint64_t, uint64_t )>();

        try {
            const auto tCompressionStart = now();
            const auto result = indexed_bzip2::compressBzip2Parallel(
                *args.inputFile, outputFileDescriptor, level, args.decoderParallelism, blockOffsetCallback );
            const auto compressionDuration = duration( tCompressionStart );
            if ( args.indexSaveFile ) {
                args.indexSaveFile->truncateToWrittenSize();
            }
            outputFile->truncate( result.compressedBytes );
            if ( args.verbose ) {
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
        if ( args.verbose ) {
            std::cerr << "Decompress " << args.inputFilePath << " -> " << args.outputFilePath << " with "
                      << args.decoderParallelism << " threads\n";
        }

        auto* const outputFile = args.primaryOutputFile();
        const auto outputFileDescriptor = outputFile ? outputFile->fd() : -1;

        const auto writeToStdErr = outputFile && outputFile->writingToStdout();
        auto& out = writeToStdErr ? std::cerr : std::cout;

        const auto offsetOutputRequested = args.indexSaveFile.has_value() || compressedOffsetsFile.has_value();

        std::unique_ptr<BZ2ReaderInterface> reader;
        if ( args.decoderParallelism == 1 ) {
            reader = std::make_unique<BZ2Reader>( std::move( args.inputFile ) );
        } else {
            reader = std::make_unique<ParallelBZ2Reader>( std::move( args.inputFile ), args.decoderParallelism,
                                                          blockFinderParallelism );
        }
        if ( args.indexLoadFile ) {
            reader->importIndex( std::move( args.indexLoadFile ) );
        }
        reader->setShowProfileOnDestruction( args.verbose );

        const auto actualBufferSize = bufferSize > 0 ? bufferSize : 1_Mi;
        std::vector<char> buffer( actualBufferSize );
        size_t nBytesDecodedTotal = 0;
        size_t nBytesWrittenTotal = 0;
        size_t newlineCount = 0;

        std::optional<size_t> lastPrintedOffset;
        size_t printedOffsetCount{ 0 };
        const auto printNewOffset =
            [&] ( const size_t compressedOffset,
                  const size_t decodedOffset )
            {
                if ( lastPrintedOffset && ( compressedOffset <= *lastPrintedOffset ) ) {
                    return;
                }
                if ( args.indexSaveFile ) {
                    writeBzip2IndexEntry(
                        [&args] ( const void* data, const size_t size ) {
                            args.indexSaveFile->write( data, size );
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
                if ( args.countLines ) {
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

        if ( args.fileRanges ) {
            for ( const auto& range : *args.fileRanges ) {
                reader->seek( static_cast<long long int>( range.offset ), SEEK_SET );
                auto remaining = range.size;
                while ( remaining > 0 ) {
                    const auto toRead = static_cast<size_t>( std::min<uint64_t>( buffer.size(), remaining ) );
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
        if ( args.indexSaveFile ) {
            args.indexSaveFile->truncateToWrittenSize();
        }

        if ( outputFile ) {
            outputFile->truncate( nBytesWrittenTotal );
            /* Close the files here to include them in the time measurement. */
            args.outputFile.reset();
            args.stdoutFile.reset();
        }

        if ( args.verbose ) {
            /* The callback already delivered every offset, so avoid another full block-map copy here.
             * In range mode, blockOffsets() would decode the whole input and could fail on corruption
             * outside the requested range. */
            const auto blockCount = offsetOutputRequested
                                    ? printedOffsetCount
                                    : ( args.fileRanges
                                        ? reader->availableBlockOffsets().size()
                                        : reader->blockOffsets().size() );
            out << "Found " << blockCount << " blocks\n";
        }

        if ( args.test ) {
            if ( !args.fileRanges ) {
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

        if ( args.countBytes || args.countLines ) {
            if ( args.countBytes && args.countLines ) {
                out << nBytesDecodedTotal << " " << newlineCount << "\n";
            } else {
                out << ( args.countBytes ? nBytesDecodedTotal : newlineCount ) << "\n";
            }
        }

        return 0;
    }

    if ( listCompressedOffsets ) {
        if ( args.verbose ) {
            std::cerr << "Find block offsets\n";
        }

        /* If effectively only the compressed offsets are requested, then we can use the blockfinder directly! */
        if ( !args.inputWasSeekable ) {
            std::cerr << "Listing compressed offsets requires seekable input. "
                         "Spool non-seekable input to a file first.\n";
            return 1;
        }
        auto sharedInputFile = ensureSharedFileReader( std::move( args.inputFile ) );

        const auto offsets = findCompressedBlocks(
            *sharedInputFile, blockFinderParallelism > 0 ? blockFinderParallelism : availablePhysicalCores(),
            static_cast<unsigned int>( bufferSize > 0 ? bufferSize : 32_Ki ) );
        dumpOffsets( offsets, [&compressedOffsetsFile] ( const void* data, const size_t size )
                     { compressedOffsetsFile->write( data, size ); } );
        compressedOffsetsFile->truncateToWrittenSize();

        if ( args.verbose ) {
            std::cout << "Found " << offsets.size() << " blocks\n";
        }
        return 0;
    }

    std::cerr << "No suitable arguments were given. Please refer to the help!\n\n";

    printIbzip2Help( options );

    return 1;
}
}


#if defined( WITH_PYTHON_SUPPORT )
using namespace indexed_bzip2;
using namespace rapidgzip;
#elif !defined( WITHOUT_MAIN )
int
main( int argc, char** argv )
{
    return rapidgzip::runCLI( [argc, argv] () { return indexed_bzip2::ibzip2CLI( argc, argv ); } );
}
#endif
