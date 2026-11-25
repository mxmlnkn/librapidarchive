#pragma once

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include <cxxopts.hpp>

#include <core/common.hpp>
#include <core/FileRanges.hpp>
#include <core/FileUtils.hpp>
#include <filereader/FileReader.hpp>
#include <filereader/Shared.hpp>
#include <filereader/SinglePass.hpp>
#include <filereader/Standard.hpp>

#include "Sandbox.hpp"
#include "thirdparty.hpp"


namespace rapidgzip
{
/**
 * Parses and validates the positional input path.
 *
 * @return The input path, where an empty string means stdin, or std::nullopt on error.
 */
[[nodiscard]] inline std::optional<std::string>
parseInputPath( const cxxopts::ParseResult& parsedArgs )
{
    if ( parsedArgs.count( "input" ) > 1 ) {
        std::cerr << "Only at most one input file is accepted!\n";
        return std::nullopt;
    }

    std::string inputFilePath;  /* Can be empty. Then, read from STDIN. */
    if ( parsedArgs.count( "input" ) == 1 ) {
        inputFilePath = parsedArgs["input"].as<std::string>();
        if ( inputFilePath == "-" ) {
            inputFilePath.clear();
        }
        if ( !inputFilePath.empty() && std::filesystem::is_directory( inputFilePath ) ) {
            std::cerr << "Input path must not be a directory: " << inputFilePath << "\n";
            return std::nullopt;
        }
        if ( !inputFilePath.empty() && !fileExists( inputFilePath ) ) {
            std::cerr << "Input file could not be found! Specified path: " << inputFilePath << "\n";
            return std::nullopt;
        }
    }

    if ( inputFilePath.empty() && !stdinHasInput() ) {
        std::cerr << "Either stdin must have input, e.g., by piping to it, or an input file must be specified!\n";
        return std::nullopt;
    }

    return inputFilePath;
}


/**
 * Strips one of the known compressed-file suffixes to deduce the decompression output path.
 * Compression must append its target suffix instead and must not call this function.
 *
 * @param warnOnFallback Print a warning when no suffix matches. The tools warn under different
 *        conditions and with different prefixes, so both are parameters.
 */
[[nodiscard]] inline std::string
deduceOutputPath( const std::string&                   inputFilePath,
                  const std::vector<std::string_view>& suffixes,
                  const bool                           warnOnFallback )
{
    for ( const auto suffix : suffixes ) {
        if ( endsWith( inputFilePath, suffix, /* case sensitive */ false ) ) {
            return std::string( inputFilePath.begin(),
                                inputFilePath.end() - static_cast<std::string::difference_type>( suffix.size() ) );
        }
    }

    const auto outputFilePath = inputFilePath + ".out";
    if ( warnOnFallback ) {
        std::cerr << "[Warning] Could not deduce output file name. Will write to '" << outputFilePath << "'\n";
    }
    return outputFilePath;
}


/* Returns whether the output path may be written, printing the shared error message otherwise. */
[[nodiscard]] inline bool
checkOutputFileWritable( const std::string& outputFilePath,
                         const bool         force )
{
    if ( force || ( outputFilePath == "/dev/null" ) || !fileExists( outputFilePath ) ) {
        return true;
    }

    std::cerr << "Output file '" << outputFilePath << "' already exists! Use --force to overwrite.\n";
    return false;
}


inline void
addCommonFileOptions( cxxopts::Options&      options,
                      const std::string_view inputSuffix,
                      const std::string_view countLinesShort )
{
    options.add_options( "Decompression" )
    ( "c,stdout", "Output to standard output. This is the default, when reading from standard input." )
    ( "f,force", "Force overwriting existing output files. "
                 "Also forces decompression even when piped to /dev/null." )
    ( "i,input", "Input file. If none is given, data is read from standard input.",
      cxxopts::value<std::string>() )
    ( "o,output",
      "Output file. If none is given, use the input file name with '" + std::string( inputSuffix )
      + "' stripped or '<input file>.out'. If no input is read from standard input and not output file "
        "is given, then will write to standard output.",
      cxxopts::value<std::string>() )
    ( "k,keep", "Keep (do not delete) input file. Only for compatibility. "
                "This tool will not delete anything automatically!" );

    options.add_options( "Output" )
    ( "h,help", "Print this help message." )
    ( "q,quiet", "Suppress non-critical error messages." )
    ( "v,verbose", "Print debug output and profiling statistics." )
    ( "V,version", "Display software version." )
    ( "oss-attributions", "Display open-source software licenses." )
    ( "oss-attributions-yaml", "Display open-source software licenses in YAML format for use with Conda." );

    const auto countLinesOption = countLinesShort.empty()
                                  ? std::string{ "count-lines" }
                                  : std::string{ countLinesShort } + ",count-lines";
    options.add_options( "Actions" )
    ( "d,decompress", "Force decompression." )
    ( "count", "Print the decompressed size." )
    ( countLinesOption, "Print the number of newline characters in the decompressed data." )
    ( "import-index", "Use an existing block offset index in the format written by --export-index.",
      cxxopts::value<std::string>() )
    ( "t,test", "Verify decompression and checksums, if not disabled with --no-verify. "
                "Note that this does not have any additional effect when combined with --decompress "
                "or --ranges because it is equivalent to --decompress -o /dev/null." );

    options.add_options( "Decompression" )
    ( "P,decoder-parallelism",
      "Use the parallel decoder or compressor. "
      "If an optional integer >= 1 is given, then that is the number of worker threads to use. "
      "Note that there might be further threads being started with light-weight work such as block finding. "
      "If 0 is given, then the parallelism will be determined automatically.",
      cxxopts::value<unsigned int>()->default_value( "0" ) )
    ( "ranges", "Decompress only the specified byte ranges. Example: 10@0,1KiB@15KiB,5L@20L,inf@40L to decompress "
                "the first 10 bytes, 1024 bytes at offset 15 KiB, as well as the 5 lines after skipping the first "
                "20 lines, and lastly, everything after skipping the first 40.",
      cxxopts::value<std::string>() );

    options.add_options( "Advanced" )
    ( "io-read-method", "Option to force a certain I/O method for reading. By default, pread will be used "
                        "when possible. Possible values: pread, sequential, locked-read",
      cxxopts::value<std::string>()->default_value( "pread" ) )
    ( "no-sandbox", "Disable the Linux process sandbox that drops privileges and restricts file and network access. "
                    "Only use this for debugging." );
}


/**
 * Registers --export-index in the "Actions" group. ibzip2 also exposes it as -L with a legacy
 * --list-offsets alias.
 */
inline void
addExportIndexOption( cxxopts::Options&      options,
                      const std::string_view exportIndexShort,
                      const std::string_view description )
{
    const auto optionNames = exportIndexShort.empty()
                             ? std::string{ "export-index" }
                             : std::string{ exportIndexShort } + ",export-index";
    options.add_options( "Actions" )
    ( optionNames, std::string{ description }, cxxopts::value<std::string>() );
}


[[nodiscard]] inline std::string
getFilePath( cxxopts::ParseResult const& parsedArgs,
             std::string          const& argument )
{
    if ( parsedArgs.count( argument ) > 1 ) {
        if ( parsedArgs.count( "quiet" ) == 0 ) {
            std::cerr << "[Warning] Multiple output files specified. Will only use the last one: "
                      << parsedArgs[argument].as<std::string>() << "!\n";
        }
    }
    if ( parsedArgs.count( argument ) > 0 ) {
        auto path = parsedArgs[argument].as<std::string>();
        if ( path != "-" ) {
            return path;
        }
    }
    return {};
}


/**
 * Prints the paths of the given file options for --verbose.
 *
 * cxxopts does not expose whether an option is registered for a tool. ParseResult::count returns zero for
 * unknown option names, so callers can also pass options that only another tool has. Only index with
 * count > 0 because operator[] throws when the option was not given.
 */
inline void
printFilePathArguments( const cxxopts::ParseResult&          parsedArgs,
                        const std::vector<std::string_view>& pathOptions )
{
    for ( const auto pathOption : pathOptions ) {
        const auto optionName = std::string{ pathOption };
        std::string path;
        if ( parsedArgs.count( optionName ) > 0 ) {
            path = parsedArgs[optionName].as<std::string>();
            if ( path.empty() || ( path == "-" ) ) {
                path = "<stdin|stdout>";
            }
        } else {
            path = "<none>";
        }
        std::cerr << "file path for " << pathOption << ": " << path << "\n";
    }
}


struct Arguments
{
    /* Flags from addCommonFileOptions. */
    bool verbose{ false };
    bool quiet{ false };
    bool force{ false };
    bool keep{ false };
    bool sandbox{ true };
    bool compress{ false };
    bool decompress{ false };
    bool test{ false };
    bool countBytes{ false };
    bool countLines{ false };
    bool writeToStdOut{ false };

    unsigned int decoderParallelism{ 0 };
    std::optional<std::vector<FileRange> > fileRanges;

    /* Paths kept for verbose messages. An empty input path means stdin, an empty output path means stdout. */
    std::string inputFilePath;
    std::string outputFilePath;

    UniqueFileReader inputFile;
    bool inputWasSeekable{ false };
    std::unique_ptr<OutputFile> outputFile;
    std::unique_ptr<OutputFile> stdoutFile;
    UniqueFileReader indexLoadFile;
    std::optional<OutputFile> indexSaveFile;

    [[nodiscard]] bool
    requestsDecodedOutput() const noexcept
    {
        return decompress || test || fileRanges.has_value();
    }

    [[nodiscard]] bool
    writesOutput() const noexcept
    {
        return outputFile || stdoutFile;
    }

    /** The output used by tools that write to exactly one stream, preferring an explicit file over stdout. */
    [[nodiscard]] OutputFile*
    primaryOutputFile() const noexcept
    {
        return outputFile ? outputFile.get() : stdoutFile.get();
    }

    [[nodiscard]] bool
    primaryOutputIsStdout() const noexcept
    {
        const auto* const output = primaryOutputFile();
        return output && output->writingToStdout();
    }
};


struct ArgumentsOptions
{
    std::string_view compressionSuffix;
    std::vector<std::string_view> decompressionSuffixes;
    /** Tool-specific file options that --verbose prints in addition to the common ones. */
    std::vector<std::string_view> additionalPathOptions;

    /** Route an index written to stdout to stderr when the tool can write decoded data to stdout at all. */
    bool indexSaveToStderrWhenStdoutFile{ false };

    /** Opens tool-specific output files before the sandbox is installed. Return false on error. */
    std::function<bool( Arguments& )> openAdditionalFiles;
};


/**
 * Parses the flags common to all actions. This does not open any file.
 *
 * @return false after printing an error. The caller should then return a nonzero exit code.
 */
[[nodiscard]] inline bool
parseCommonArguments( Arguments& args, const cxxopts::ParseResult& parsedArgs )
{
    args.force = parsedArgs["force"].as<bool>();
    args.verbose = parsedArgs["verbose"].as<bool>();
    args.quiet = parsedArgs["quiet"].as<bool>();
    args.sandbox = parsedArgs.count( "no-sandbox" ) == 0;
    args.decoderParallelism = parsedArgs["decoder-parallelism"].as<unsigned int>();

    args.countBytes = parsedArgs.count( "count" ) > 0;
    args.countLines = parsedArgs.count( "count-lines" ) > 0;
    args.test = parsedArgs.count( "test" ) > 0;
    args.writeToStdOut =
        ( parsedArgs.count( "stdout" ) > 0 )
        || ( ( parsedArgs.count( "output" ) > 0 ) && ( parsedArgs["output"].as<std::string>() == "-" ) );
    /* count() is safe for tools that do not register --compress, so the shared conflict checks can already
     * run before rapidgzip gains compression support. */
    args.compress = parsedArgs.count( "compress" ) > 0;
    args.decompress = parsedArgs.count( "decompress" ) > 0;

    if ( parsedArgs.count( "ranges" ) > 0 ) {
        args.fileRanges = parseFileRanges( parsedArgs["ranges"].as<std::string>() );
    }

    if ( args.compress
         && ( args.decompress || args.test || args.countBytes || args.countLines
              || args.fileRanges.has_value()
              || ( parsedArgs.count( "import-index" ) > 0 )
              || ( parsedArgs.count( "list-compressed-offsets" ) > 0 ) ) ) {
        std::cerr << "Compression cannot be combined with decompression, test, count, range, "
                  << "import-index, or list-compressed-offsets options.\n";
        return false;
    }

    const auto& ioReadMethod = parsedArgs["io-read-method"].as<std::string>();
    if ( ( ioReadMethod != "sequential" ) && ( ioReadMethod != "locked-read" ) && ( ioReadMethod != "pread" ) ) {
        std::cerr << "Unknown I/O read method: " << ioReadMethod << "\n";
        return false;
    }

    return true;
}


/** Opens the raw input. The --io-read-method wrapper is applied only after the sandbox is installed. */
[[nodiscard]] inline bool
openInputFile( Arguments& args, const cxxopts::ParseResult& parsedArgs )
{
    const auto inputFilePath = parseInputPath( parsedArgs );
    if ( !inputFilePath ) {
        return false;
    }
    args.inputFilePath = *inputFilePath;
    args.inputFile = openFileOrStdin( args.inputFilePath );
    args.inputWasSeekable = args.inputFile->seekable();
    return true;
}


/** The wrappers may start a reader thread, so create them only after the restrictions are active. */
inline void
applyIoReadMethod( Arguments& args, const cxxopts::ParseResult& parsedArgs )
{
    const auto& ioReadMethod = parsedArgs["io-read-method"].as<std::string>();
    if ( ioReadMethod == "sequential" ) {
        args.inputFile = std::make_unique<SinglePassFileReader>( std::move( args.inputFile ) );
    } else {
        auto sharedFile = ensureSharedFileReader( std::move( args.inputFile ) );
        sharedFile->setUsePread( ioReadMethod == "pread" );
        args.inputFile = std::move( sharedFile );
    }
}


inline void
installSandbox( const Arguments& args )
{
    if ( args.sandbox ) {
        sandbox::installIfEnabled( { {}, args.verbose, args.quiet } );
    }
}


/** Input-only variant for specialized actions such as --analyze. Does not resolve or open any output file. */
[[nodiscard]] inline bool
parseInputArguments( Arguments& args, const cxxopts::ParseResult& parsedArgs )
{
    return parseCommonArguments( args, parsedArgs ) && openInputFile( args, parsedArgs );
}


/**
 * Resolves and opens all output and index files. The sandbox is installed only after this returns.
 */
[[nodiscard]] inline bool
openOutputFiles( Arguments&                  args,
                 const cxxopts::ParseResult& parsedArgs,
                 const ArgumentsOptions&     options )
{
    /* --import-index */

    if ( parsedArgs.count( "import-index" ) > 0 ) {
        const auto path = parsedArgs["import-index"].as<std::string>();
        if ( !fileExists( path ) ) {
            std::cerr << "The index to import was not found: " << path << "\n";
            return false;
        }
        args.indexLoadFile = std::make_unique<StandardFileReader>( path );
    }

    /* --export-index */

    std::optional<std::string> indexSavePath;
    if ( parsedArgs.count( "export-index" ) > 0 ) {
        indexSavePath = getFilePath( parsedArgs, "export-index" );
        if ( !indexSavePath->empty() && !checkOutputFileWritable( *indexSavePath, args.force ) ) {
            return false;
        }
    }

    if ( args.indexLoadFile && !indexSavePath && ( args.decoderParallelism == 1 ) && !args.quiet ) {
        std::cerr << "[Warning] The index only has an effect for parallel decoding and index exporting.\n";
    }

    args.outputFilePath = getFilePath( parsedArgs, "output" );

    const auto writeOutput = args.compress
                             || ( args.requestsDecodedOutput()
                                  && ( !args.test || args.writeToStdOut || !args.outputFilePath.empty() ) );
    if ( args.compress ) {
        /* Compression appends the target suffix. It must not strip an existing input suffix. */
        if ( !args.writeToStdOut && args.outputFilePath.empty() && !args.inputFilePath.empty() ) {
            args.outputFilePath = args.inputFilePath + std::string( options.compressionSuffix );
        }
    } else if ( writeOutput && !args.writeToStdOut && args.outputFilePath.empty() && !args.inputFilePath.empty() ) {
        args.outputFilePath = deduceOutputPath( args.inputFilePath, options.decompressionSuffixes,
                                                /* warnOnFallback */ !args.quiet );
    }

    if ( writeOutput && !args.outputFilePath.empty() && !checkOutputFileWritable( args.outputFilePath, args.force ) ) {
        return false;
    }

    if ( args.verbose ) {
        printFilePathArguments( parsedArgs, { "input", "output", "import-index", "export-index" } );
        printFilePathArguments( parsedArgs, options.additionalPathOptions );
    }

    if ( writeOutput ) {
        if ( args.writeToStdOut || ( args.outputFilePath.empty() && args.inputFilePath.empty() ) ) {
            args.stdoutFile = std::make_unique<OutputFile>( /* empty path implies stdout */ "" );
        }
        if ( !args.outputFilePath.empty() ) {
            args.outputFile = std::make_unique<OutputFile>( args.outputFilePath );
        }
    }

    if ( indexSavePath ) {
        const auto useStderr = options.indexSaveToStderrWhenStdoutFile
                               ? static_cast<bool>( args.stdoutFile )
                               : args.primaryOutputIsStdout();
        args.indexSaveFile.emplace( *indexSavePath, useStderr );
    }

    if ( options.openAdditionalFiles && !options.openAdditionalFiles( args ) ) {
        return false;
    }

    return true;
}


/**
 * Parses the common options and opens the input, output, and index files.
 *
 * The files must be opened here so that a later sandbox can deny all remaining path access.
 *
 * @return false after printing an error. The caller should then return a nonzero exit code.
 */
[[nodiscard]] inline bool
parseArguments( Arguments& args, const cxxopts::ParseResult& parsedArgs, const ArgumentsOptions& options )
{
    if ( !parseCommonArguments( args, parsedArgs )
         || !openInputFile( args, parsedArgs )
         || !openOutputFiles( args, parsedArgs, options ) ) {
        return false;
    }
    installSandbox( args );
    applyIoReadMethod( args, parsedArgs );
    return true;
}


/** Input-only variant for specialized actions such as --analyze. Does not resolve or open any output file. */
[[nodiscard]] inline bool
parseInputArguments( Arguments& args, const cxxopts::ParseResult& parsedArgs )
{
    if ( !parseCommonArguments( args, parsedArgs ) || !openInputFile( args, parsedArgs ) ) {
        return false;
    }
    installSandbox( args );
    applyIoReadMethod( args, parsedArgs );
    return true;
}


/**
 * Handles --help, --version, and the OSS attribution actions.
 *
 * @param versionText The complete line to print for --version, including the trailing newline.
 * @return The exit code when an action was requested, otherwise std::nullopt.
 */
template<typename HelpPrinter, typename Libraries>
[[nodiscard]] std::optional<int>
handleInformationalAction( const cxxopts::ParseResult& parsedArgs,
                           const std::string_view      ossRootName,
                           const std::string_view      versionText,
                           const Libraries&            libraries,
                           HelpPrinter&&               printHelp )
{
    if ( parsedArgs.count( "help" ) > 0 ) {
        printHelp();
        return 0;
    }

    if ( parsedArgs.count( "version" ) > 0 ) {
        std::cout << versionText;
        return 0;
    }

    if ( parsedArgs.count( "oss-attributions" ) > 0 ) {
        thirdparty::printOssAttributions( std::cout, libraries );
        return 0;
    }

    if ( parsedArgs.count( "oss-attributions-yaml" ) > 0 ) {
        thirdparty::printOssAttributionsYaml( std::cout, ossRootName, libraries );
        return 0;
    }

    return std::nullopt;
}


/**
 * Wraps a CLI entry point and translates the exceptions that all tools handle identically.
 *
 * @param truncatedStreamMessage Appended to the generic "Unexpected end of file." prefix, e.g.,
 *        "Truncated or invalid bzip2?".
 */
template<typename CLIFunction>
int
runCLI( CLIFunction&& cli )
{
    try {
        return cli();
    } catch ( const EndOfFileReached& ) {
        std::cerr << "Unexpected end of file. Truncated or invalid compressed stream?\n";
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
}
}  // namespace rapidgzip
