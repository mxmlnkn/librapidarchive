#pragma once

#include <cstddef>
#include <map>
#include <ostream>


namespace indexed_bzip2
{
using BlockOffsets = std::map<size_t, size_t>;


inline void
writeBzip2Index( std::ostream&       out,
                 const BlockOffsets& offsets )
{
    if ( !out.good() ) {
        return;
    }

    for ( const auto& [compressedOffset, uncompressedOffset] : offsets ) {
        out << compressedOffset << "," << uncompressedOffset << "\n";
    }
}
}  // namespace indexed_bzip2
