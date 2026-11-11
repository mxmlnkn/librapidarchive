#pragma once

#include <cstddef>
#include <map>
#include <string_view>

namespace bzip2
{
constexpr auto MAGIC_BITS_BLOCK = 0x314159265359ULL;  /* bcd(pi) */
constexpr auto MAGIC_BITS_EOS = 0x177245385090ULL;    /* bcd(sqrt(pi)) */
constexpr auto MAGIC_BITS_SIZE = 48;
constexpr std::string_view MAGIC_BYTES_BZ2 = "BZh";

/**
 * @return Whether @p offsets is the canonical single-entry index of an empty bzip2 stream.
 *
 * An empty stream has no data block, so its only block entry is the EOS block. Callers may use either the
 * EOS bit offset or zero, which means "look after the stream header". The decoded offset must be zero;
 * otherwise this is a truncated index that starts at a later EOS, which is not an empty stream.
 */
template<typename BitReader>
[[nodiscard]] inline bool
isEmptyBzip2BlockMap( const std::map<std::size_t, std::size_t>& offsets,
                      BitReader&                                bitReader )
{
    if ( offsets.size() != 1 ) {
        return false;
    }
    const auto& [compressedOffset, decodedOffset] = *offsets.begin();
    if ( ( decodedOffset != 0 ) || !bitReader.seekable() ) {
        return false;
    }

    const auto compressedBits = bitReader.size();
    if ( !compressedBits ) {
        return false;
    }
    if ( *compressedBits == 0 ) {
        return compressedOffset == 0;
    }

    const auto magicOffset = compressedOffset == 0 ? std::size_t{ 32 } : compressedOffset;
    if ( magicOffset + MAGIC_BITS_SIZE > *compressedBits ) {
        return false;
    }

    const auto oldPosition = bitReader.tell();
    bitReader.seek( static_cast<long long int>( magicOffset ) );
    const auto firstMagic = bitReader.read( MAGIC_BITS_SIZE );
    bitReader.seek( static_cast<long long int>( oldPosition ) );
    return firstMagic == MAGIC_BITS_EOS;
}
}  // namespace bzip2
