#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <sys/stat.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxopts.hpp>

#include <core/AffinityHelpers.hpp>
#include <core/common.hpp>
#include <core/FileRanges.hpp>
#include <core/FileUtils.hpp>
#include <core/Statistics.hpp>
#include <rapidgzip/gzip/AnalyzeFeatures.hpp>
#include <rapidgzip/gzip/GzipAnalyzer.hpp>
#include <rapidgzip/rapidgzip.hpp>

#include "CLIHelper.hpp"
#include "thirdparty.hpp"


namespace rapidgzip
{
struct RapidgzipArguments :
    public Arguments
{
    size_t chunkSize{ 4_Mi };
    std::optional<uint64_t> maxDecompressedChunkSize;

    bool keepIndex{ false };
    bool windowSparsity{ true };
    bool gatherLineOffsets{ false };
    bool crc32Enabled{ true };

    IndexFormat indexFormat{ IndexFormat::INDEXED_GZIP };
    deflate::AnalyzeFeatureSet analyzeFeatures{ deflate::AnalyzeFeatureSet::all() };
};


void
printRapidgzipHelp( const cxxopts::Options& options )
{
    std::cout
    << options.help()
    << "\n"
    << "If no file names are given, rapidgzip decompresses from standard input to standard output.\n"
    << "\n"
    << "Examples:\n"
    << "\n"
    << "Decompress a file:\n"
    << "  rapidgzip -d file.gz\n"
    << "\n"
    << "Decompress a file in parallel:\n"
    << "  rapidgzip -d -P 0 file.gz\n"
    << "\n"
    << "List information about all gzip streams and deflate blocks:\n"
    << "  rapidgzip --analyze file.gz\n"
    << "\n"
    << "List only block and Huffman statistics:\n"
    << "  rapidgzip --analyze --analyze-features block-stats,huffman-stats file.gz\n"
    << std::endl;
}


template<typename Reader>
void
printIndexAnalytics( const Reader& reader )
{
    const auto offsets = reader->blockOffsets();
    if ( offsets.size() <= 1 ) {
        return;
    }

    /* Analyze the offsets. */

    Statistics<double> encodedOffsetSpacings;
    Statistics<double> decodedOffsetSpacings;
    for ( auto it = offsets.begin(), nit = std::next( offsets.begin() ); nit != offsets.end(); ++it, ++nit ) {
        const auto& [encodedOffset, decodedOffset] = *it;
        const auto& [nextEncodedOffset, nextDecodedOffset] = *nit;
        if ( nextEncodedOffset - encodedOffset > 0 ) {
            encodedOffsetSpacings.merge( static_cast<double>( nextEncodedOffset - encodedOffset ) / CHAR_BIT / 1e6 );
            decodedOffsetSpacings.merge( static_cast<double>( nextDecodedOffset - decodedOffset ) / 1e6 );
        }
    }

    std::cerr
        << "[Seekpoints Index]\n"
        << "    Encoded offset spacings: ( min: " << encodedOffsetSpacings.min << ", "
        << encodedOffsetSpacings.formatAverageWithUncertainty()
        << ", max: " << encodedOffsetSpacings.max << " ) MB\n"
        << "    Decoded offset spacings: ( min: " << decodedOffsetSpacings.min << ", "
        << decodedOffsetSpacings.formatAverageWithUncertainty()
        << ", max: " << decodedOffsetSpacings.max << " ) MB\n";

    /* Analyze the windows. */
    const auto gzipIndex = reader->gzipIndex();
    if ( gzipIndex.windows ) {
        const auto [lock, windows] = gzipIndex.windows->data();
        const auto compressedWindowSize =
            std::accumulate(
                windows->begin(), windows->end(), size_t( 0 ),
                [] ( size_t sum, const auto& kv ) {
                    return sum + ( kv.second ? kv.second->compressedSize() : 0 );
                } );
        const auto decompressedWindowSize =
            std::accumulate(
                windows->begin(), windows->end(), size_t( 0 ),
                [] ( size_t sum, const auto& kv ) {
                    return sum + ( kv.second ? kv.second->decompressedSize() : 0 );
                } );
        std::cerr << "    Windows Count: " << windows->size() << "\n"
                  << "    Total Compressed Window Size: " << formatBytes( compressedWindowSize ) << "\n"
                  << "    Total Decompressed Window Size: " << formatBytes( decompressedWindowSize ) << "\n";
    }

    const auto& newlineOffsets = reader->newlineOffsets();
    if ( !newlineOffsets.empty() ) {
        /* The newline offsets in the index must be sorted! This is a sanity check for debugging. */
        const auto lessLineOffset = [] ( const auto& a, const auto& b ) { return a.lineOffset < b.lineOffset; };
        const auto sorted = std::is_sorted( newlineOffsets.begin(), newlineOffsets.end(), lessLineOffset );
        std::cerr << "    Newline Offsets Sorted: " << std::boolalpha << sorted << "\n";

        const auto maxLineOffset = std::accumulate(
            newlineOffsets.begin(), newlineOffsets.end(), uint64_t( 0 ), [] ( auto a, auto offset ) {
                return std::max( a, offset.lineOffset );
            } );
        std::cerr << "    Largest Newline Offset: " << maxLineOffset << "\n";
    }
}


enum class DecompressErrorCode
{
    SUCCESS = 0,
    BROKEN_PIPE = 1,
};


template<typename ChunkData,
         typename ReadFunctor = std::function<size_t( std::unique_ptr<ParallelGzipReader<ChunkData> >& )> >
[[nodiscard]] DecompressErrorCode
decompressParallel( RapidgzipArguments& args,
                    const ReadFunctor&  readFunctor )
{
    using Reader = ParallelGzipReader<ChunkData>;

    auto reader = std::make_unique<Reader>( std::move( args.inputFile ), args.decoderParallelism, args.chunkSize );

    const auto gatherLineOffsets =
        args.gatherLineOffsets || ( args.indexSaveFile && ( args.indexFormat == IndexFormat::GZTOOL_WITH_LINES ) );

    reader->setStatisticsEnabled( args.verbose );
    reader->setShowProfileOnDestruction( args.verbose );
    if ( args.maxDecompressedChunkSize ) {
        reader->setMaxDecompressedChunkSize( *args.maxDecompressedChunkSize );
    }
    reader->setCRC32Enabled( args.crc32Enabled );
    reader->setKeepIndex( args.indexSaveFile || args.indexLoadFile || args.keepIndex );
    reader->setWindowSparsity( args.windowSparsity );
    reader->setNewlineCharacter( gatherLineOffsets ? std::make_optional( '\n' ) : std::nullopt );
    if ( ( args.indexFormat == IndexFormat::GZTOOL ) || ( args.indexFormat == IndexFormat::GZTOOL_WITH_LINES ) ) {
        /* Compress with zlib instead of gzip to avoid recompressions when exporting the index. */
        reader->setWindowCompressionType( CompressionType::ZLIB );
    }

    const auto hadImportedIndex = static_cast<bool>( args.indexLoadFile );
    if ( args.indexLoadFile ) {
        reader->importIndex( std::move( args.indexLoadFile ) );
        if ( args.verbose ) {
            printIndexAnalytics( reader );
        }
    }

    /* If we have file ranges referencing newlines we have to gather newlines before calling readFunctor. */
    const auto hasNewlineRange = args.fileRanges && std::any_of(
        args.fileRanges->begin(), args.fileRanges->end(),
        [] ( const auto& range ) { return ( range.size > 0 ) && ( range.offsetIsLine || range.sizeIsLine ); } );
    if ( gatherLineOffsets && hasNewlineRange ) {
        reader->gatherLineOffsets();
    }

    try {
        readFunctor( reader );
    } catch ( const BrokenPipeException& ) {
        return DecompressErrorCode::BROKEN_PIPE;
    }

    /* Ensure that we have the line offsets because the readFunctor is not required to read the whole file. */
    if ( gatherLineOffsets ) {
        reader->gatherLineOffsets();
    }

    if ( args.indexSaveFile ) {
        const auto checkedWrite =
            [&args] ( const void* buffer, size_t size )
            {
                args.indexSaveFile->write( buffer, size );
            };

        reader->exportIndex( checkedWrite, args.indexFormat );
        args.indexSaveFile->truncateToWrittenSize();
    }

    if ( args.verbose && !hadImportedIndex && args.indexSaveFile ) {
        printIndexAnalytics( reader );
    }

    return DecompressErrorCode::SUCCESS;
}


int
rapidgzipCLI( int                  argc,
              char const * const * argv )
{
    /* Cleaned, checked, and typed arguments. */
    RapidgzipArguments args;

    /**
     * @note For some reason implicit values do not mix very well with positional parameters!
     *       Parameters given to arguments with implicit values will be matched by the positional argument instead!
     */
    cxxopts::Options options( "rapidgzip",
                              "A gzip decompressor tool based on the rapidgzip backend from ratarmount" );
    addCommonFileOptions( options, ".gz", /* countLinesShort */ "l" );

    options.add_options( "Advanced" )
    ( "chunk-size", "The chunk size decoded by the parallel workers in KiB.",
      cxxopts::value<unsigned int>()->default_value( "4096" ) )
    ( "max-decompressed-chunk-size",
      "Stop decoding a chunk preemptively when it decompresses to more than this many bytes. Defaults to 20 times "
      "the chunk size. Sizes accept SI and binary suffixes, e.g. 64Mi.",
      cxxopts::value<std::string>() )
    ( "verify", "Verify CRC32 checksum during decompression. Will slow down decompression and there are already "
                "some implicit and explicit checks like whether the end of the file could be reached and whether "
                "the stream size is correct. If no action is specified, this will do nothing. Use --test to "
                "force checksum verification.",
      cxxopts::value( args.crc32Enabled )->implicit_value( "true" ) )
    ( "no-verify", "Do not verify CRC32 checksum during decompression. Might speed up decompression and there are "
                   "already some implicit and explicit checks like whether the end of the file could be reached "
                   "and whether the stream size is correct.",
      cxxopts::value( args.crc32Enabled )->implicit_value( "false" ) )
    ( "index-format", "Option to select an output index format. Possible values: gztool, gztool-with-lines, "
                      "indexed_gzip.",
      cxxopts::value<std::string>()->default_value( "indexed_gzip" ) )
    ( "sparse-windows", "On by default. Reduce index compressibility by zeroing out window data that is not "
                        "referenced in the subsequent stream.",
      cxxopts::value( args.windowSparsity )->implicit_value( "true" ) )
    ( "no-sparse-windows", "Do not zero out unreferenced data in index windows. This may very slightly improve "
                           "first-time decompression speed, but may also lead to higher memory usage. It can also "
                           "be useful to create exported index that are binary identical to other those created "
                           "by other tools such as gztool to debug indexes. In normal usage the sparsity is an "
                           "index modification that is fully compatible with the original tools.",
      cxxopts::value( args.windowSparsity )->implicit_value( "false" ) );

    /* These options are offered because just piping to other tools can already bottleneck everything! */
    const auto analyzeFeaturesHelp =
        std::string( "Comma-separated list of features to print with --analyze (implies --analyze). Default: all. "
                     "Use '+'/'-' prefixes to add or remove from the default set. Features: " )
        + deflate::analyzeFeatureNameList()
        + " (block-details is an alias for block-info). "
          "Example: +huffman-stats,-block-info. A plain list such as block-stats,huffman-stats "
          "prints only statistics.";
    addExportIndexOption( options, /* exportIndexShort */ "",
                          "Write out a block offset index file in the format selected with --index-format." );
    options.add_options( "Actions" )
    ( "analyze", "Print output about the internal file format structure like the block types. "
                 "Select sections with --analyze-features." )
    ( "analyze-features", analyzeFeaturesHelp, cxxopts::value<std::string>() );

    options.parse_positional( { "input" } );

    /* cxxopts allows to specify arguments multiple times. But if the argument type is not a vector, then only
     * the last value will be kept! Therefore, do not check against this usage and simply use that value.
     * Arguments may only be queried with as if they have (default) values. */

    const auto parsedArgs = options.parse( argc, argv );

    /* Check against simple commands like help and version. */

    const auto informationalExitCode = handleInformationalAction(
        parsedArgs, "rapidgzip",
        "rapidgzip, CLI to the parallelized, indexed, and seekable gzip decoding library rapidgzip version "
            + std::to_string( static_cast<uint32_t>( rapidgzip::VERSION[0] ) ) + "."
            + std::to_string( static_cast<uint32_t>( rapidgzip::VERSION[1] ) ) + "."
            + std::to_string( static_cast<uint32_t>( rapidgzip::VERSION[2] ) ) + "\n",
        std::array{
            thirdparty::CXXOPTS,
        #ifdef LIBRAPIDARCHIVE_WITH_ISAL
            thirdparty::ISAL,
        #endif
        #ifdef LIBRAPIDARCHIVE_WITH_RPMALLOC
            thirdparty::RPMALLOC,
        #endif
            thirdparty::ZLIB,
        #ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
            thirdparty::BZIP2,
        #endif
            thirdparty::LIBSAIS,
            thirdparty::TOYBOX,
        },
        [&options] () { printRapidgzipHelp( options ); } );
    if ( informationalExitCode ) {
        return *informationalExitCode;
    }

    /* Specialized input-only actions such as --analyze do not need any output or index parsing. */
    const auto analyze = ( parsedArgs.count( "analyze" ) > 0 ) || ( parsedArgs.count( "analyze-features" ) > 0 );
    if ( analyze ) {
        if ( parsedArgs.count( "analyze-features" ) > 0 ) {
            args.analyzeFeatures = deflate::parseAnalyzeFeatures( parsedArgs["analyze-features"].as<std::string>() );
        }
        if ( !parseInputArguments( args, parsedArgs ) ) {
            return 1;
        }
        return deflate::analyze( std::move( args.inputFile ), args.verbose, args.analyzeFeatures ) == Error::NONE
               ? 0
               : 1;
    }

    /* Validate the index format before parseArguments creates any output file. */
    if ( parsedArgs.count( "index-format" ) > 0 ) {
        const auto indexFormatString = parsedArgs["index-format"].as<std::string>();
        if ( indexFormatString == "indexed_gzip" ) {
            args.indexFormat = IndexFormat::INDEXED_GZIP;
        } else if ( indexFormatString == "gztool" ) {
            args.indexFormat = IndexFormat::GZTOOL;
        } else if ( indexFormatString == "gztool-with-lines" ) {
            args.indexFormat = IndexFormat::GZTOOL_WITH_LINES;
        } else {
            throw std::invalid_argument( "Invalid index format string: " + indexFormatString );
        }
    }

    ArgumentsOptions argumentsOptions;
    argumentsOptions.decompressionSuffixes = { ".bgzf", ".bgz", ".gzip", ".gz", ".zlib", ".deflate", ".bzip2", ".bz2" };
    argumentsOptions.indexSaveToStderrWhenStdoutFile = true;

    /* Parse action arguments. */

    if ( !parseArguments( args, parsedArgs, argumentsOptions ) ) {
        return 1;
    }

    if ( args.decoderParallelism <= 0 ) {
        args.decoderParallelism = availableCores();
    }
    args.decompress = args.requestsDecodedOutput();

    /* Parse ranges. */
    if ( args.fileRanges ) {
        /* Check whether the index needs to be kept because the ranges do not traverse the file in order. */
        if ( args.fileRanges->size() > 1 ) {
            for ( size_t i = 0; i + 1 < args.fileRanges->size(); ++i ) {
                const auto& current = args.fileRanges->at( i );
                const auto& next = args.fileRanges->at( i + 1 );
                /* Avoid offset + size overflow for size == UINT64_MAX, which represents 'inf'. */
                if ( ( next.offset < current.offset )
                     || ( current.size > next.offset - current.offset ) ) {
                    args.keepIndex = true;
                    break;
                }
            }
        }

        /* Check whether some ranges are lines and enable line offset gathering in that case. */
        for ( const auto& range : *args.fileRanges ) {
            if ( ( range.size > 0 ) && ( range.offsetIsLine || range.sizeIsLine ) ) {
                /* Because we cannot arbitrarily convert lines to offsets, we cannot easily determine whether
                 * backward seeking is necessary. Therefore keep the index if line offsets are used. */
                args.keepIndex = true;
                args.gatherLineOffsets = true;
                break;
            }
        }
    }

    /* Parse index arguments. */

    if ( ( parsedArgs.count( "index-format" ) > 0 ) && !args.indexSaveFile && !args.quiet ) {
        std::cerr << "[Warning] The index format has no effect without --export-index!\n";
    }

    /* Check that there is at least one actionable request. */

    if ( !args.decompress && !args.countBytes && !args.countLines && !args.indexSaveFile ) {
        std::cerr << "No suitable arguments were given. Please refer to the help!\n\n";
        printRapidgzipHelp( options );
        return 1;
    }

    /* Actually do things as requested. */

    const auto outputFileDescriptor = args.outputFile ? args.outputFile->fd() : -1;
    const auto stdoutFileDescriptor = args.stdoutFile ? args.stdoutFile->fd() : -1;

    /* The CLI owns one splice session for its stdout stream. The session destructor leaks the final tail
     * because the reader can still reference it after the writer has finished. */
    SpliceVault* stdoutSpliceVault = nullptr;
#ifdef LIBRAPIDARCHIVE_WITH_VMSPLICE
    std::unique_ptr<SpliceVault> stdoutSpliceVaultOwner;
    if ( stdoutFileDescriptor != -1 ) {
        stdoutSpliceVaultOwner = std::make_unique<SpliceVault>( stdoutFileDescriptor );
        stdoutSpliceVault = stdoutSpliceVaultOwner.get();
    }
#endif

    if ( args.decompress && !args.test && args.verbose ) {
        std::cerr << "Decompress " << ( args.inputFilePath.empty() ? "<stdin>" : args.inputFilePath.c_str() ) << " -> ";
        if ( args.stdoutFile ) {
            std::cerr << "<stdout>";
            if ( args.outputFile ) {
                std::cerr << ", ";
            }
        }
        if ( args.outputFile ) {
            std::cerr << args.outputFilePath;
        }
        std::cerr << "\n";
    }

    uint64_t newlineCount{ 0 };

    const auto t0 = now();
    double writeDuration{ 0 };

    /* The same stream can be reachable through stdout and the output file. Write it only once. */
    std::vector<int> outputFileDescriptors;
    for ( const auto fileDescriptor : { stdoutFileDescriptor, outputFileDescriptor } ) {
        if ( fileDescriptor != -1 ) {
            outputFileDescriptors.push_back( fileDescriptor );
        }
    }
    deduplicateFileDescriptors( outputFileDescriptors );

    size_t totalBytesRead{ 0 };
    const auto writeAndCount =
        [stdoutFileDescriptor, stdoutSpliceVault, &args, &outputFileDescriptors,
         &newlineCount, &totalBytesRead, &writeDuration]
        ( const std::shared_ptr<ChunkData>& chunkData,
          size_t const                      offsetInChunk,
          size_t const                      dataToWriteSize )
        {
            for ( const auto fileDescriptor : outputFileDescriptors ) {
                const auto tWriteStart = now();
                auto* const spliceVault = ( fileDescriptor == stdoutFileDescriptor ) ? stdoutSpliceVault : nullptr;
                const auto errorCode = writeAll( chunkData, fileDescriptor, offsetInChunk, dataToWriteSize,
                                                 spliceVault );
                writeDuration += duration( tWriteStart );
                if ( errorCode == EPIPE ) {
                    throw BrokenPipeException();
                }

                if ( errorCode != 0 ) {
                    std::stringstream message;
                    message << "Failed to write all bytes because of: " << strerror( errorCode )
                            << " (" << errorCode << ")";
                    throw std::runtime_error( std::move( message ).str() );
                }
            }

            totalBytesRead += dataToWriteSize;

            if ( args.countLines ) {
                using deflate::DecodedData;
                for ( auto it = DecodedData::Iterator( *chunkData, offsetInChunk, dataToWriteSize );
                      static_cast<bool>( it ); ++it )
                {
                    const auto& [buffer, size] = *it;
                    newlineCount += countNewlines( { reinterpret_cast<const char*>( buffer ), size } );
                }
            }
        };

    args.chunkSize = parsedArgs["chunk-size"].as<unsigned int>() * 1_Ki;
    if ( parsedArgs.count( "max-decompressed-chunk-size" ) > 0 ) {
        args.maxDecompressedChunkSize = parseByteSize( parsedArgs["max-decompressed-chunk-size"].as<std::string>() );
    }

    auto errorCode = DecompressErrorCode::SUCCESS;
    const auto hasOutputFiles = ( outputFileDescriptor != -1 ) || ( stdoutFileDescriptor != -1 );
#ifndef LIBRAPIDARCHIVE_WITH_ISAL
    const auto useSequentialFastPath =
        ( args.decoderParallelism == 1 )
        && !args.fileRanges
        && !args.indexLoadFile
        && !args.indexSaveFile
        && !args.verbose
        && !args.gatherLineOffsets;
#else
    constexpr bool useSequentialFastPath = false;
#endif
    if ( useSequentialFastPath ) {
        /**
         * Straight sequential decode without the block finder, chunk scheduling, and chunk windows. This is
         * used for the common single-worker case where none of the pipeline-only features (index, seeking,
         * ranges, line offsets, statistics) are requested. With ISA-L enabled the chunk decoder can pass
         * larger buffers to the full ISA-L inflate state machine, which is faster than the per-symbol
         * ISA-L Huffman path used by GzipReader, so the fast path is only taken for the custom decoder.
         */
        rapidgzip::GzipReader reader( std::move( args.inputFile ) );
        reader.setCRC32Enabled( args.crc32Enabled );

        const auto writeFunctor =
            [&args, &outputFileDescriptors, &newlineCount, &totalBytesRead, &writeDuration]
            ( const void* const buffer, const uint64_t size )
            {
                for ( const auto fileDescriptor : outputFileDescriptors ) {
                    const auto writeStart = now();
                    const auto errorCode = writeAllToFd( fileDescriptor, buffer, size );
                    writeDuration += duration( writeStart );
                    if ( errorCode == EPIPE ) {
                        throw BrokenPipeException();
                    }
                    if ( errorCode != 0 ) {
                        std::stringstream message;
                        message << "Failed to write all bytes because of: " << strerror( errorCode )
                                << " (" << errorCode << ")";
                        throw std::runtime_error( std::move( message ).str() );
                    }
                }

                totalBytesRead += size;
                if ( args.countLines ) {
                    newlineCount += countNewlines(
                        { reinterpret_cast<const char*>( buffer ), static_cast<size_t>( size ) } );
                }
            };

        try {
            reader.read( writeFunctor );
        } catch ( const BrokenPipeException& ) {
            errorCode = DecompressErrorCode::BROKEN_PIPE;
        }
    } else if ( !args.indexSaveFile && args.countBytes && !args.countLines && !args.decompress && !hasOutputFiles ) {
        /* Need to do nothing with the chunks because decompressParallel returns the decompressed size.
         * Note that we use ChunkDataCounter to speed up decompression. Therefore an index
         * will not be created and there also will be no checksum verification! */
        args.crc32Enabled = false;
        errorCode = decompressParallel<ChunkDataCounter>(
            args, [&totalBytesRead] ( const auto& reader ) {
                totalBytesRead = reader->seek( 0, SEEK_END );
            } );
    } else {
        const auto readRange =
            [&totalBytesRead, hasOutputFiles, &args, &writeAndCount]
            ( const auto&  reader,
              const size_t size )
            {
                /* An empty functor will lead to decompression to be skipped if the index is finalized! */
                if ( hasOutputFiles || args.countLines ) {
                    reader->read( writeAndCount, size );
                } else {
                    totalBytesRead += reader->read( /* do nothing */ nullptr, size );
                }
            };

        const auto readLines =
            [&] ( const auto&  reader,
                  const size_t lineCount,
                  const auto&  writeFunctor )
            {
                size_t lastLineOffset = reader->tell();
                const auto newlineCharacter = reader->newlineCharacter().value_or( '\n' );
                auto remainingLineCount = lineCount;

                const auto forwardLineWrites =
                    [&lastLineOffset, &remainingLineCount, newlineCharacter, &writeFunctor]
                    ( const std::shared_ptr<ChunkData>& chunkData,
                      const size_t                      offsetInChunk,
                      const size_t                      dataToWriteSize )
                    {
                        if ( remainingLineCount == 0 ) {
                            return;
                        }

                        size_t nBytesRead{ 0 };
                        for ( auto it = deflate::DecodedData::Iterator( *chunkData, offsetInChunk, dataToWriteSize );
                              static_cast<bool>( it ); ++it )
                        {
                            const auto& [buffer, size] = *it;
                            const auto result = findNthNewline( { reinterpret_cast<const char*>( buffer ), size },
                                                                remainingLineCount, newlineCharacter );
                            remainingLineCount = result.remainingLineCount;
                            if ( remainingLineCount == 0 ) {
                                if ( result.position == std::string_view::npos ) {
                                    throw std::logic_error( "Find n-th line should return a valid position when "
                                                            "the input line count was not 0 but is 0 thereafter." );
                                }

                                /* Skip over last newline character with +1. */
                                nBytesRead += result.position + 1U;
                                lastLineOffset += result.position + 1U;
                                break;
                            }
                            nBytesRead += size;
                            lastLineOffset += size;
                        }

                        if ( nBytesRead > dataToWriteSize ) {
                            throw std::logic_error( "Shouldn't have read more bytes than specified in the chunk." );
                        }

                        writeFunctor( chunkData, offsetInChunk, nBytesRead );
                    };

                while ( remainingLineCount > 0 ) {
                    const auto currentBytesRead = reader->read( forwardLineWrites, 4_Mi );
                    if ( currentBytesRead == 0 ) {
                        break;
                    }
                }

                reader->seekTo( lastLineOffset );
            };

        errorCode = decompressParallel<ChunkData>(
            args, [&] ( const auto& reader ) {
                if ( !args.fileRanges ) {
                    if ( !hasOutputFiles && args.countLines && !reader->newlineOffsets().empty() ) {
                        const auto& newlineOffset = reader->newlineOffsets().back();
                        newlineCount = newlineOffset.lineOffset;
                        totalBytesRead = reader->seekTo( newlineOffset.uncompressedOffsetInBytes );
                        /* Strictly speaking, we should be able to return here, but in case the newline offset
                         * semantics change or when only partial index should be loaded (not supported yet), then
                         * the subsequent "read until file end" should cover these cases. */
                    }
                    readRange( reader, std::numeric_limits<size_t>::max() );
                    return;
                }

                for ( const auto& range : *args.fileRanges ) {
                    if ( range.size == 0 ) {
                        continue;
                    }

                    if ( ( ( range.offsetIsLine && ( range.offset > 0 ) ) || range.sizeIsLine )
                         && reader->newlineOffsets().empty() )
                    {
                        throw std::logic_error( "There should be newline offset information at this point!" );
                    }

                    /* Seek to line or byte offset. Note that line 0 starts at byte 0 by definition. */
                    if ( range.offsetIsLine && ( range.offset > 0 ) ) {
                        const auto& newlineOffsets = reader->newlineOffsets();
                        const auto lessLineOffset =
                            [] ( const auto& a, const auto lineOffset ) { return a.lineOffset < lineOffset; };
                        auto newlineOffset = std::lower_bound( newlineOffsets.begin(), newlineOffsets.end(),
                                                               range.offset, lessLineOffset );
                        if ( newlineOffset == newlineOffsets.end() ) {
                            continue;
                        }

                        if ( newlineOffset == newlineOffsets.begin() ) {
                            throw std::logic_error( "Bisection may never point to the first element because "
                                                    "line offset 0 is always smaller than any line offset handled "
                                                    "in this branch!" );
                        }
                        --newlineOffset;

                        reader->seekTo( newlineOffset->uncompressedOffsetInBytes );

                        if ( newlineOffset->lineOffset >= range.offset ) {
                            throw std::logic_error( "Bisection should have returned a line offset prior to the "
                                                    "one we need to seek to." );
                        }

                        readLines( reader, range.offset - newlineOffset->lineOffset,
                                   [] ( const auto&, size_t, size_t ) {} );
                    } else {
                        reader->seekTo( range.offset );
                    }

                    if ( range.sizeIsLine ) {
                        readLines( reader, range.size, writeAndCount );
                    } else {
                        readRange( reader, range.size );
                    }
                }
            } );
    }

    const auto writeToStdErr = ( args.stdoutFile && args.stdoutFile->writingToStdout() )
                               || ( args.outputFile && args.outputFile->writingToStdout() )
                               || ( args.indexSaveFile && args.indexSaveFile->writingToStdout() );
    if ( args.outputFile ) {
        args.outputFile->truncate( totalBytesRead );
        args.outputFile.reset();  // Close the file here to include it in the time measurement.
    }

    const auto t1 = now();
    if ( args.verbose ) {
        std::cerr << "Spent " << writeDuration << " s writing to output.\n";
        std::cerr << "Decompressed in total " << totalBytesRead << " B in " << duration( t0, t1 ) << " s -> "
                  << static_cast<double>( totalBytesRead ) / 1e6 / duration( t0, t1 ) << " MB/s\n";
    }

    auto& out = writeToStdErr ? std::cerr : std::cout;
    if ( args.countBytes != args.countLines ) {
        out << ( args.countBytes ? totalBytesRead : newlineCount ) << "\n";
    } else if ( args.countBytes && args.countLines ) {
        out << "Size: " << totalBytesRead << "\n";
        out << "Lines: " << newlineCount << "\n";
    }

    switch ( errorCode )
    {
    case DecompressErrorCode::BROKEN_PIPE:
        return 128 + /* SIGPIPE */ 13;
    case DecompressErrorCode::SUCCESS:
        return 0;
    }
    return 2;
}
}


#if defined( WITH_PYTHON_SUPPORT )
using namespace rapidgzip;
#elif !defined( WITHOUT_MAIN )
int
main( int argc, char** argv )
{
    return rapidgzip::runCLI( [argc, argv] () { return rapidgzip::rapidgzipCLI( argc, argv ); } );
}
#endif
