#pragma once

#ifdef LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include <bzlib.h>

#include <core/FileUtils.hpp>
#include <core/ThreadPool.hpp>
#include <filereader/FileReader.hpp>


namespace indexed_bzip2
{
struct BrokenPipeException :
    public std::exception
{
    [[nodiscard]] const char*
    what() const noexcept override
    {
        return "Broken pipe";
    }
};


struct Bzip2CompressionResult
{
    uint64_t uncompressedBytes{ 0 };
    uint64_t compressedBytes{ 0 };
};


/**
 * Compresses one independent bzip2 stream. The result is a complete stream with its own header and footer,
 * so complete streams from multiple workers can be concatenated.
 */
[[nodiscard]] inline std::vector<uint8_t>
compressBzip2Stream( const uint8_t* const input,
                     const size_t         size,
                     const int            level )
{
    if ( ( level < 1 ) || ( level > 9 ) ) {
        throw std::invalid_argument( "bzip2 compression level must be in the range 1..9." );
    }

    std::vector<uint8_t> compressed( size + size / 100U + 600U );
    auto compressedSize = static_cast<unsigned int>( compressed.size() );
    /* libbz2 rejects a null source pointer even for an empty input, so provide a valid dummy address. */
    static constexpr char EMPTY_INPUT = 0;
    const auto* const source = size > 0 ? reinterpret_cast<const char*>( input ) : &EMPTY_INPUT;
    const auto result = BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char*>( compressed.data() ), &compressedSize,
        const_cast<char*>( source ), static_cast<unsigned int>( size ),
        level, /* verbosity */ 0, /* workFactor */ 0 );
    if ( result != BZ_OK ) {
        throw std::runtime_error( "bzip2 compression failed with error code " + std::to_string( result ) );
    }
    compressed.resize( compressedSize );
    return compressed;
}


/**
 * Compresses @p inputFile into independent bzip2 streams in parallel and writes them in input order.
 *
 * The bzip2 level's block-size limit applies to the RLE1-encoded data, not to the raw input. RLE1's
 * worst case is a run of exactly four identical bytes:
 *
 *     raw:  A A A A
 *     RLE1: A A A A <count=0>
 *
 * which expands four bytes to five, i.e., at most 5/4. Keeping the raw chunk at 80% of the level's
 * block size therefore leaves 25% headroom and avoids a full block followed by a tiny tail block when
 * the expansion crosses the limit.
 */
[[nodiscard]] inline Bzip2CompressionResult
compressBzip2Parallel( rapidgzip::FileReader& inputFile,
                       const int              outputFileDescriptor,
                       const int              level,
                       const size_t           parallelism )
{
    if ( ( level < 1 ) || ( level > 9 ) ) {
        throw std::invalid_argument( "bzip2 compression level must be in the range 1..9." );
    }
    if ( outputFileDescriptor < 0 ) {
        throw std::invalid_argument( "A valid output file descriptor is required for bzip2 compression." );
    }

    const auto blockSize = static_cast<size_t>( level ) * 100000U * 4U / 5U;
    rapidgzip::ThreadPool pool( std::max<size_t>( 1, parallelism ) );
    const auto maxInFlight = std::max<size_t>( 1, pool.capacity() * 2 );
    std::deque<std::future<std::vector<uint8_t> > > pending;
    Bzip2CompressionResult result;

    const auto writeData =
        [outputFileDescriptor, &result] ( const std::vector<uint8_t>& data ) {
            const auto errorCode = rapidgzip::writeAllToFd( outputFileDescriptor, data.data(), data.size() );
            if ( errorCode == EPIPE ) {
                throw BrokenPipeException();
            }
            if ( errorCode != 0 ) {
                throw std::runtime_error( "Failed to write compressed data because of: "
                                          + std::string( strerror( errorCode ) )
                                          + " (" + std::to_string( errorCode ) + ")" );
            }
            result.compressedBytes += data.size();
        };

    const auto writePending = [&writeData] ( auto& future ) { writeData( future.get() ); };

    std::vector<uint8_t> block( blockSize );
    bool wroteStream{ false };
    while ( true ) {
        size_t bytesRead{ 0 };
        while ( bytesRead < block.size() ) {
            const auto nBytesRead = inputFile.read(
                reinterpret_cast<char*>( block.data() + bytesRead ), block.size() - bytesRead );
            if ( nBytesRead == 0 ) {
                break;
            }
            bytesRead += nBytesRead;
        }
        if ( bytesRead == 0 ) {
            break;
        }

        result.uncompressedBytes += bytesRead;
        std::vector<uint8_t> data( block.begin(), block.begin() + bytesRead );
        pending.emplace_back(
            pool.submit(
                [data = std::move( data ), level] () {
                    return compressBzip2Stream( data.data(), data.size(), level );
                } ) );
        wroteStream = true;

        while ( pending.size() > maxInFlight ) {
            writePending( pending.front() );
            pending.pop_front();
        }

        if ( bytesRead < block.size() ) {
            break;
        }
    }

    while ( !pending.empty() ) {
        writePending( pending.front() );
        pending.pop_front();
    }

    /* An empty input still needs one valid bzip2 stream so that the output is a valid empty archive. */
    if ( !wroteStream ) {
        writeData( compressBzip2Stream( nullptr, 0, level ) );
    }

    return result;
}
}  // namespace indexed_bzip2

#endif  // LIBRAPIDARCHIVE_WITH_BZIP2_COMPRESSION
