#pragma once

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>

#include <cxxopts.hpp>

#include <filereader/FileReader.hpp>

#include "thirdparty.hpp"


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
    } catch ( const rapidgzip::EndOfFileReached& ) {
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
