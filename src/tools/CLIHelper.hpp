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
 * Handles --help, --version, and the OSS attribution actions.
 *
 * @param versionText The complete line to print for --version, including the trailing newline.
 * @return The exit code when an action was requested, otherwise std::nullopt.
 */
template<typename HelpPrinter, typename Libraries>
[[nodiscard]] std::optional<int>
handleInformationalAction( const cxxopts::ParseResult&             parsedArgs,
                           const std::string_view                  ossRootName,
                           const std::string_view                  versionText,
                           const Libraries&                        libraries,
                           HelpPrinter&&                           printHelp )
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
runCLI( CLIFunction&&           cli )
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
