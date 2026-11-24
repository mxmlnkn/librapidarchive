#pragma once

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>

#include <cxxopts.hpp>

#include <core/common.hpp>
#include <core/FileRanges.hpp>
#include <core/FileUtils.hpp>
#include <filereader/FileReader.hpp>

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
deduceOutputPath( const std::string&                            inputFilePath,
                  const std::initializer_list<std::string_view> suffixes,
                  const bool                                    warnOnFallback )
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
    if ( ( outputFilePath == "/dev/null" ) || !fileExists( outputFilePath ) || force ) {
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


struct Arguments
{
    /* Flags from addCommonFileOptions. */
    bool verbose{ false };
    bool quiet{ false };
    bool force{ false };
    bool keep{ false };
    bool sandbox{ true };
    bool decompress{ false };
    bool test{ false };
    bool countBytes{ false };
    bool countLines{ false };
    bool writeToStdOut{ false };

    unsigned int decoderParallelism{ 0 };
    std::optional<std::vector<FileRange> > fileRanges;

    /* Resolved paths. An empty input path means stdin, an empty output path means stdout. */
    std::string inputFilePath;
    std::string outputFilePath;
    std::string indexLoadPath;
    std::string indexSavePath;

    UniqueFileReader inputFile;
    std::unique_ptr<OutputFile> outputFile;
    std::unique_ptr<OutputFile> stdoutFile;
    UniqueFileReader indexLoadFile;
    std::optional<OutputFile> indexSaveFile;
};


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
