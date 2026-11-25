#pragma once

#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <core/common.hpp>

#include "ChunkData.hpp"
#include "gzip/adler32.hpp"
#include "gzip/crc32.hpp"
#include "gzip/gzip.hpp"


namespace rapidgzip
{
/**
 * Verifies the per-stream tail fields of a chunk: the container checksum and, for gzip, ISIZE.
 *
 * The running checksums are kept across chunks, so this state must be reset whenever seeking skips data.
 * gzip and zlib both have a footer per stream, but zlib's Adler-32 is recomputed from the decoded chunk
 * while gzip's CRC32 is maintained incrementally by @ref ChunkData because it is a hot path. External
 * per-deflate-stream CRC32s (used for raw deflate streams) are checked independently of the container.
 */
class ChecksumVerifier
{
public:
    void
    setEnabled( bool enabled ) noexcept
    {
        m_crc32.setEnabled( enabled );
    }

    [[nodiscard]] bool
    enabled() const noexcept
    {
        return m_crc32.enabled();
    }

    [[nodiscard]] uint64_t
    verifiedCRC32Count() const noexcept
    {
        return m_verifiedCRC32Count;
    }

    [[nodiscard]] uint64_t
    verifiedAdler32Count() const noexcept
    {
        return m_verifiedAdler32Count;
    }

    void
    setDeflateStreamCRC32s( std::unordered_map<size_t, uint32_t> crc32s )
    {
        m_deflateStreamCRC32s = std::move( crc32s );
    }

    void
    addDeflateStreamCRC32( const size_t   endOfStreamOffsetInBytes,
                           const uint32_t crc32 )
    {
        m_deflateStreamCRC32s.insert_or_assign( endOfStreamOffsetInBytes, crc32 );
    }

    /**
     * Resetting is necessary after seeking or replacing the block map because the data between the previous
     * and the new position may have been skipped. Reading from offset 0 starts at a known stream start and can
     * be verified from scratch.
     */
    void
    resetState( const size_t position )
    {
        m_checksumStateValid = position == 0;
        m_nextCRC32ChunkOffset = 0;
        m_crc32.reset();
        m_adler32.reset();
        m_lastStreamEndDecodedOffset = position == 0 ? std::optional<size_t>( 0 ) : std::nullopt;
        m_lastProcessedChecksumChunkOffset = std::numeric_limits<size_t>::max();
        m_lastProcessedISIZEChunkOffset = std::numeric_limits<size_t>::max();
    }

    /**
     * The first chunk may start after the container header. Knowing the first block offset lets the verifier
     * recognize that chunk as a stream continuation instead of treating it as a partial stream.
     */
    [[nodiscard]] bool
    needsFirstBlockOffset( const ChunkData& chunkData ) const noexcept
    {
        return enabled() && ( m_nextCRC32ChunkOffset == 0 ) && !chunkData.crc32s.empty()
               && ( m_lastProcessedChecksumChunkOffset != chunkData.encodedOffsetInBits );
    }

    /**
     * Verifies all stream ends inside @p chunkData. May be called multiple times for the same chunk; each
     * chunk is processed at most once so that the running state and the verification counters stay consistent.
     */
    void
    processChunk( const ChunkData&            chunkData,
                  const size_t                chunkDecodedOffset,
                  const std::optional<size_t> firstBlockEncodedOffset = std::nullopt )
    {
        if ( hasUncompressedSizeField( chunkData.configuration.fileType ) ) {
            processStreamSizes( chunkData, chunkDecodedOffset );
        }

        if ( !enabled() || chunkData.crc32s.empty() ) {
            return;
        }

        const auto chunkStart = chunkData.encodedOffsetInBits;
        const auto chunkEnd = chunkStart + chunkData.encodedSizeInBits;

        if ( m_lastProcessedChecksumChunkOffset == chunkStart ) {
            return;
        }

        /* Use local copies and commit them only after all verifications succeeded. Otherwise, a caller retrying
         * after a checksum error would either double-count the chunk or skip verification entirely. */
        auto crc32 = m_crc32;
        auto adler32 = m_adler32;
        auto checksumStateValid = m_checksumStateValid;
        auto nextCRC32ChunkOffset = m_nextCRC32ChunkOffset;
        auto verifiedCRC32Count = m_verifiedCRC32Count;
        auto verifiedAdler32Count = m_verifiedAdler32Count;

        if ( ( nextCRC32ChunkOffset == 0 ) && firstBlockEncodedOffset ) {
            nextCRC32ChunkOffset = *firstBlockEncodedOffset;
        }

        /* As long as CRC32 is enabled, this should not happen and we filtered above for !enabled(). */
        if ( chunkData.crc32s.size() != chunkData.footers.size() + 1 ) {
            throw std::logic_error( "Fewer CRC32s in chunk than expected based on the gzip footers!" );
        }

        const auto totalCRC32StreamSize = std::accumulate(
            chunkData.crc32s.begin(), chunkData.crc32s.end(), size_t( 0 ),
            [] ( size_t sum, const auto& calculator ) { return sum + calculator.streamSize(); } );
        if ( totalCRC32StreamSize != chunkData.decodedSizeInBytes ) {
            std::stringstream message;
            message << "CRC32 computation stream size (" << formatBytes( totalCRC32StreamSize ) << ") differs from "
                    << "chunk size: " << formatBytes( chunkData.decodedSizeInBytes ) << "!\n"
                    << "Please open an issue or disable integrated CRC32 verification as a quick workaround.";
            throw std::logic_error( std::move( message ).str() );
        }

        const auto streamChecksum = checksumType( chunkData.configuration.fileType );
        const auto isContinuation = checksumStateValid && ( nextCRC32ChunkOffset == chunkStart );
        /* A chunk at the beginning of the file starts at a known stream start. */
        const auto startsAtStreamStart = isContinuation || ( chunkDecodedOffset == 0 );

        if ( isContinuation ) {
            crc32.append( chunkData.crc32s.front() );
        } else {
            /* Start over from this chunk's first segment. It may be a partial stream and is therefore only
             * verified below if it is known to be complete. */
            crc32 = chunkData.crc32s.front();
        }

        const auto firstFooterOffset =
            chunkData.footers.empty() ? chunkData.decodedSizeInBytes
                                      : chunkData.footers.front().blockBoundary.decodedOffset;
        if ( streamChecksum == ChecksumType::ADLER32 ) {
            if ( !isContinuation ) {
                adler32.reset();
            }
            updateAdler32( adler32, chunkData, 0, firstFooterOffset );
        }

        bool canVerifyNextStream = startsAtStreamStart;
        size_t previousFooterOffset = 0;
        for ( size_t i = 0; i < chunkData.footers.size(); ++i ) {
            const auto& footer = chunkData.footers[i];
            const auto footerByteOffset = ceilDiv( footer.blockBoundary.encodedOffset, CHAR_BIT );

            if ( ( streamChecksum == ChecksumType::ADLER32 ) && ( i > 0 ) ) {
                adler32.reset();
                updateAdler32( adler32, chunkData, previousFooterOffset,
                               footer.blockBoundary.decodedOffset - previousFooterOffset );
            }

            /* For the first footer in an out-of-sync chunk, the stream may have started before this chunk.
             * For gzip, its ISIZE tells us whether the stream started exactly at the chunk start. */
            const auto streamFullyContained =
                canVerifyNextStream || ( i > 0 )
                || ( ( streamChecksum == ChecksumType::CRC32 )
                     && ( ( chunkDecodedOffset + footer.blockBoundary.decodedOffset ) < ( 1ULL << 32U ) )
                     && ( footer.blockBoundary.decodedOffset == footer.gzipFooter.uncompressedSize ) );

            if ( streamFullyContained ) {
                bool verified = false;
                if ( const auto externalCRC32 = m_deflateStreamCRC32s.find( footerByteOffset );
                     externalCRC32 != m_deflateStreamCRC32s.end() )
                {
                    crc32.verify( externalCRC32->second );
                    verified = true;
                }
                /* An external CRC does not replace a gzip footer CRC if both are present. */
                if ( streamChecksum == ChecksumType::CRC32 ) {
                    crc32.verify( footer.gzipFooter.crc32 );
                    verified = true;
                }
                if ( verified ) {
                    verifiedCRC32Count++;
                }
                if ( streamChecksum == ChecksumType::ADLER32 ) {
                    adler32.verify( footer.zlibFooter.adler32 );
                    verifiedAdler32Count++;
                }
            }

            previousFooterOffset = footer.blockBoundary.decodedOffset;
            crc32 = chunkData.crc32s.at( i + 1 );
            canVerifyNextStream = true;
        }

        if ( ( streamChecksum == ChecksumType::ADLER32 ) && !chunkData.footers.empty() ) {
            adler32.reset();
            updateAdler32( adler32, chunkData, previousFooterOffset,
                           chunkData.decodedSizeInBytes - previousFooterOffset );
        }

        nextCRC32ChunkOffset = chunkEnd;
        /* After a footer, the trailing stream starts within this chunk and is verifiable in later chunks.
         * If there was no footer, only a known stream start makes the trailing stream verifiable. */
        checksumStateValid = canVerifyNextStream;

        m_crc32 = crc32;
        m_adler32 = adler32;
        m_verifiedCRC32Count = verifiedCRC32Count;
        m_verifiedAdler32Count = verifiedAdler32Count;
        m_nextCRC32ChunkOffset = nextCRC32ChunkOffset;
        m_checksumStateValid = checksumStateValid;
        m_lastProcessedChecksumChunkOffset = chunkStart;
    }

private:
    static void
    updateAdler32( Adler32Calculator& adler32,
                   const ChunkData&   chunkData,
                   const size_t       offset,
                   const size_t       size )
    {
        for ( auto it = deflate::DecodedData::Iterator( chunkData, offset, size ); it; ++it ) {
            const auto& [buffer, bufferSize] = *it;
            adler32.update( buffer, bufferSize );
        }
    }

    /**
     * The gzip ISIZE field is a modulo-2^32 copy of the stream's decoded size. GzipChunk only checks it when it
     * also read the stream header, which is never the case for the first member because its header was consumed
     * during file type detection, and it cannot check streams spanning multiple chunks. Use the global decoded
     * offsets here instead, which works independently of chunk splitting and of the decoder used.
     */
    void
    processStreamSizes( const ChunkData& chunkData,
                        const size_t     chunkDecodedOffset )
    {
        /* read() may be called multiple times for the same chunk. Process its footers only once. */
        if ( m_lastProcessedISIZEChunkOffset == chunkData.encodedOffsetInBits ) {
            return;
        }

        /* Do not update the member before all footers have been verified, so that a caller retrying after a
         * verification error processes this chunk again instead of silently skipping it. */
        auto lastStreamEndDecodedOffset = m_lastStreamEndDecodedOffset;
        for ( size_t i = 0; i < chunkData.footers.size(); ++i ) {
            const auto& footer = chunkData.footers[i];
            const auto streamEnd = chunkDecodedOffset + footer.blockBoundary.decodedOffset;

            std::optional<size_t> streamStart;
            if ( i == 0 ) {
                /* May be unknown after seeking into the middle of a stream. A chunk at decoded offset 0
                 * and every BGZF block start are known stream starts. */
                if ( lastStreamEndDecodedOffset ) {
                    streamStart = lastStreamEndDecodedOffset;
                } else if ( ( chunkDecodedOffset == 0 )
                            || ( ( ( chunkDecodedOffset + footer.blockBoundary.decodedOffset ) < ( 1ULL << 32U ) )
                                 && ( footer.blockBoundary.decodedOffset == footer.gzipFooter.uncompressedSize ) ) ) {
                    streamStart = chunkDecodedOffset;
                }
            } else {
                streamStart = chunkDecodedOffset + chunkData.footers[i - 1].blockBoundary.decodedOffset;
            }

            if ( streamStart ) {
                const auto streamSize = streamEnd - *streamStart;
                /* ISIZE is stored modulo 2^32. */
                if ( static_cast<uint32_t>( streamSize ) != footer.gzipFooter.uncompressedSize ) {
                    std::stringstream message;
                    message << "Mismatching size (" << streamSize << " <-> footer: "
                            << footer.gzipFooter.uncompressedSize << ") for gzip stream!";
                    throw std::runtime_error( std::move( message ).str() );
                }
            }
            lastStreamEndDecodedOffset = streamEnd;
        }

        m_lastStreamEndDecodedOffset = lastStreamEndDecodedOffset;
        m_lastProcessedISIZEChunkOffset = chunkData.encodedOffsetInBits;
    }

private:
    CRC32Calculator m_crc32;
    Adler32Calculator m_adler32;
    /* Whether the running checksums still cover all data since the last known stream start. */
    bool m_checksumStateValid{ true };
    size_t m_lastProcessedChecksumChunkOffset{ std::numeric_limits<size_t>::max() };
    /* Global decoded offset at which the last seen gzip stream ended. Unknown after seeking into a stream. */
    std::optional<size_t> m_lastStreamEndDecodedOffset{ 0 };
    size_t m_lastProcessedISIZEChunkOffset{ std::numeric_limits<size_t>::max() };
    uint64_t m_nextCRC32ChunkOffset{ 0 };
    uint64_t m_verifiedCRC32Count{ 0 };
    uint64_t m_verifiedAdler32Count{ 0 };
    std::unordered_map<size_t, uint32_t> m_deflateStreamCRC32s;
};
}  // namespace rapidgzip
