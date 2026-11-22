#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>


namespace rapidgzip::deflate
{
enum class AnalyzeFeature : uint8_t
{
    BLOCK_INFO,
    HUFFMAN_INFO,
    BLOCK_STATS,
    HUFFMAN_STATS,
    STREAM_INFO,
    PROFILE,
    COUNT,
};

constexpr size_t ANALYZE_FEATURE_COUNT = static_cast<size_t>( AnalyzeFeature::COUNT );

struct AnalyzeFeatureInfo
{
    AnalyzeFeature feature;
    std::string_view name;
};

constexpr std::array<AnalyzeFeatureInfo, ANALYZE_FEATURE_COUNT> ANALYZE_FEATURES{ {
    { AnalyzeFeature::BLOCK_INFO, "block-info" },
    { AnalyzeFeature::HUFFMAN_INFO, "huffman-info" },
    { AnalyzeFeature::BLOCK_STATS, "block-stats" },
    { AnalyzeFeature::HUFFMAN_STATS, "huffman-stats" },
    { AnalyzeFeature::STREAM_INFO, "stream-info" },
    { AnalyzeFeature::PROFILE, "profile" },
} };


class AnalyzeFeatureSet
{
public:
    constexpr AnalyzeFeatureSet() = default;

    [[nodiscard]] static constexpr AnalyzeFeatureSet
    all()
    {
        AnalyzeFeatureSet result;
        for ( auto& enabled : result.m_enabled ) {
            enabled = true;
        }
        return result;
    }

    [[nodiscard]] constexpr bool
    contains( const AnalyzeFeature feature ) const
    {
        return m_enabled.at( static_cast<size_t>( feature ) );
    }

    constexpr void
    set( const AnalyzeFeature feature,
         const bool           enabled = true )
    {
        m_enabled.at( static_cast<size_t>( feature ) ) = enabled;
    }

    [[nodiscard]] constexpr bool
    operator==( const AnalyzeFeatureSet& other ) const
    {
        for ( size_t i = 0; i < m_enabled.size(); ++i ) {
            if ( m_enabled[i] != other.m_enabled[i] ) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr bool
    operator!=( const AnalyzeFeatureSet& other ) const
    {
        return !( *this == other );
    }

private:
    std::array<bool, ANALYZE_FEATURE_COUNT> m_enabled{};
};


[[nodiscard]] inline std::string
analyzeFeatureNameList()
{
    std::string result;
    for ( const auto& info : ANALYZE_FEATURES ) {
        if ( !result.empty() ) {
            result += ", ";
        }
        result += info.name;
    }
    return result;
}


[[nodiscard]] inline AnalyzeFeature
parseAnalyzeFeature( const std::string_view name )
{
    for ( const auto& info : ANALYZE_FEATURES ) {
        if ( info.name == name ) {
            return info.feature;
        }
    }

    if ( name == "block-details" ) {
        return AnalyzeFeature::BLOCK_INFO;
    }

    throw std::invalid_argument( "Unknown analyze feature '" + std::string( name ) + "'. Valid features are: "
                                 + analyzeFeatureNameList() + " (block-details is an alias for block-info)." );
}


/**
 * @param specification An empty string selects all features. Otherwise, a comma-separated list of feature names.
 *        A list without prefixes selects exactly the named features. If every entry has a '+' or '-' prefix, then
 *        the default set of all features is modified instead. Mixing prefixed and unprefixed entries is not allowed.
 */
[[nodiscard]] inline AnalyzeFeatureSet
parseAnalyzeFeatures( const std::string_view specification )
{
    if ( trimWhitespace( specification ).empty() ) {
        return AnalyzeFeatureSet::all();
    }

    std::vector<std::string_view> tokens;
    size_t tokenStart = 0;
    while ( tokenStart <= specification.size() ) {
        auto tokenEnd = specification.find( ',', tokenStart );
        if ( tokenEnd == std::string_view::npos ) {
            tokenEnd = specification.size();
        }

        const auto token = trimWhitespace( specification.substr( tokenStart, tokenEnd - tokenStart ) );
        if ( token.empty() ) {
            throw std::invalid_argument( "Empty analyze feature in list: '" + std::string( specification ) + "'." );
        }
        tokens.emplace_back( token );

        if ( tokenEnd == specification.size() ) {
            break;
        }
        tokenStart = tokenEnd + 1;
    }

    bool hasPrefixedTokens{ false };
    bool hasUnprefixedTokens{ false };
    for ( const auto token : tokens ) {
        if ( ( token.front() == '+' ) || ( token.front() == '-' ) ) {
            hasPrefixedTokens = true;
        } else {
            hasUnprefixedTokens = true;
        }
    }

    if ( hasPrefixedTokens && hasUnprefixedTokens ) {
        throw std::invalid_argument( "Analyze features must either be a plain selection or all use '+'/'-' prefixes." );
    }

    auto result = hasPrefixedTokens ? AnalyzeFeatureSet::all() : AnalyzeFeatureSet{};
    for ( auto token : tokens ) {
        char prefix = '\0';
        if ( ( token.front() == '+' ) || ( token.front() == '-' ) ) {
            prefix = token.front();
            token.remove_prefix( 1 );
            token = trimWhitespace( token );
        }

        const auto feature = parseAnalyzeFeature( token );
        result.set( feature, prefix != '-' );
    }

    return result;
}
}  // namespace rapidgzip::deflate
