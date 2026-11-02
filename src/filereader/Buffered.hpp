#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <core/AlignedAllocator.hpp>
#include <core/common.hpp>
#include <core/VectorView.hpp>

#include "FileReader.hpp"


namespace rapidgzip
{
class BufferedFileReader :
    public FileReader
{
public:
    using AlignedBuffer = AlignedVector<char>;

public:
    explicit
    BufferedFileReader( UniqueFileReader fileReader,
                        size_t           bufferSize = 128_Ki ) :
        m_maxBufferSize( bufferSize ),
        m_file( std::move( fileReader ) )
    {}

    explicit
    BufferedFileReader( const std::vector<char>& inMemoryFileContents,
                        size_t                   bufferSize = 128_Ki ) :
        m_maxBufferSize( bufferSize ),
        m_buffer( inMemoryFileContents.begin(), inMemoryFileContents.end() )
    {}

    explicit
    BufferedFileReader( const VectorView<char>& inMemoryFileContents,
                        size_t                  bufferSize = 128_Ki ) :
        m_maxBufferSize( bufferSize ),
        m_buffer( inMemoryFileContents.begin(), inMemoryFileContents.end() )
    {}

    explicit
    BufferedFileReader( AlignedBuffer inMemoryFileContents,
                        size_t        bufferSize = 128_Ki ) :
        m_maxBufferSize( bufferSize ),
        m_buffer( std::move( inMemoryFileContents ) )
    {}

    [[nodiscard]] UniqueFileReader
    cloneRaw() const override
    {
        std::unique_ptr<BufferedFileReader> clone;
        if ( m_file ) {
            clone = std::make_unique<BufferedFileReader>( m_file->clone() );
        } else {
            clone = std::make_unique<BufferedFileReader>( m_buffer, m_maxBufferSize );
        }
        clone->m_closed = m_closed;
        return clone;
    }

    /* Copying is simply not allowed because that might interfere with the file position state, use SharedFileReader! */

    void
    close() override
    {
        m_closed = true;
        if ( m_file ) {
            m_file->close();
        }
        m_buffer.clear();
    }

    [[nodiscard]] bool
    closed() const override
    {
        /* An empty in-memory buffer is EOF, not a closed reader. Only close() sets m_closed. */
        return m_closed || ( m_file && m_file->closed() && m_buffer.empty() );
    }

    [[nodiscard]] bool
    eof() const override
    {
        if ( m_offset < bufferEnd() ) {
            return false;
        }

        if ( !m_file ) {
            return true;
        }

        if ( m_file->closed() ) {
            return m_file->eof();
        }

        /* For seekable files, the file size describes EOF independently of the underlying position, which may
         * still be ahead of m_offset when a retained cached range is read after a direct read. */
        if ( m_file->seekable() ) {
            if ( const auto fileSize = m_file->size(); fileSize.has_value() ) {
                return m_offset >= *fileSize;
            }
        }

        /* Unseekable readers only report EOF after a read hit the end. Just as importantly, m_offset may lie
         * before the end of the skipped range when the underlying reader has already observed EOF. */
        return m_file->eof() && ( m_offset >= m_file->tell() );
    }

    [[nodiscard]] bool
    fail() const override
    {
        return m_file && m_file->fail();
    }

    [[nodiscard]] int
    fileno() const override
    {
        if ( m_file ) {
            return m_file->fileno();
        }
        throw std::invalid_argument( "Trying to get fileno of an in-memory or closed file!" );
    }

    [[nodiscard]] bool
    seekable() const override
    {
        return !m_file || m_file->seekable();
    }

    [[nodiscard]] size_t
    read( char*  buffer,
          size_t nMaxBytesToRead ) override
    {
        if ( closed() ) {
            throw ReaderClosed();
        }

        if ( nMaxBytesToRead == 0 ) {
            return 0;
        }

        /* Read from buffer as much as possible. */
        auto nBytesRead = readFromBuffer( buffer, nMaxBytesToRead );
        if ( nBytesRead >= nMaxBytesToRead ) {
            return nBytesRead;
        }

        /* If we cannot refill the buffer, then that was it. */
        if ( !m_file ) {
            return nBytesRead;
        }

        /* Skip buffering step for very large reads. */
        if ( nMaxBytesToRead - nBytesRead >= m_maxBufferSize ) {
            if ( m_file->tell() != m_offset ) {
                m_file->seekTo( m_offset );
            }
            const auto nBytesReadFromFile = m_file->read( buffer == nullptr ? buffer : buffer + nBytesRead,
                                                          nMaxBytesToRead - nBytesRead );
            m_offset += nBytesReadFromFile;
            return nBytesRead + nBytesReadFromFile;
        }

        refillBuffer();
        /* Passing a null buffer means skipping bytes. Do not derive a destination pointer from it, otherwise
         * the copy performed by readFromBuffer writes to a small invalid address. */
        return nBytesRead + readFromBuffer( buffer == nullptr ? buffer : buffer + nBytesRead,
                                            nMaxBytesToRead - nBytesRead );
    }

    size_t
    seek( long long int offset,
          int           origin = SEEK_SET ) override
    {
        if ( closed() ) {
            throw ReaderClosed();
        }

        const auto newOffset = effectiveOffset( offset, origin );

        /* Check if the buffer already covers the requested offset. The buffer start is not zero and does not
         * move when the logical offset leaves the cached range. */
        if ( ( newOffset >= m_originalBufferOffset ) && ( newOffset <= bufferEnd() ) ) {
            m_offset = newOffset;
            return tell();
        }

        if ( !m_file ) {
            throw std::invalid_argument( "Cannot seek outside of in-memory file range!" );
        }

        /* Position the underlying file as before, but keep the cached range intact. */
        m_offset = m_file->seek( static_cast<long long int>( newOffset ), SEEK_SET );

        return tell();
    }

    [[nodiscard]] std::optional<size_t>
    size() const override
    {
        return m_file ? m_file->size() : m_buffer.size();
    }

    [[nodiscard]] size_t
    tell() const override
    {
        return m_offset;
    }

    void
    clearerr() override
    {
        if ( m_file ) {
            m_file->clearerr();
        }
    }

private:
    [[nodiscard]] size_t
    bufferEnd() const
    {
        return m_originalBufferOffset + m_buffer.size();
    }

    void
    refillBuffer()
    {
        if ( !m_file ) {
            throw std::invalid_argument( "Cannot refill buffer for buffer-only file!" );
        }

        if ( m_file->tell() != m_offset ) {
            m_file->seekTo( m_offset );
        }
        m_originalBufferOffset = m_offset;
        m_buffer.resize( m_maxBufferSize );
        const auto nBytesRead = m_file->read( m_buffer.data(), m_buffer.size() );
        m_buffer.resize( nBytesRead );
    }

    size_t
    readFromBuffer( char*  buffer,
                    size_t nMaxBytesToRead )
    {
        if ( ( m_offset < m_originalBufferOffset ) || ( m_offset >= bufferEnd() ) ) {
            return 0;
        }

        const auto nBytesToReadFromBuffer = std::min( bufferEnd() - m_offset, nMaxBytesToRead );
        if ( buffer != nullptr ) {
            std::memcpy( buffer, m_buffer.data() + ( m_offset - m_originalBufferOffset ), nBytesToReadFromBuffer );
        }
        m_offset += nBytesToReadFromBuffer;
        return nBytesToReadFromBuffer;
    }

protected:
    const size_t m_maxBufferSize;
    UniqueFileReader m_file;

    /**
     * The buffer is a read-through cache for [m_originalBufferOffset, m_originalBufferOffset + m_buffer.size()).
     * m_offset is the absolute file offset of the next byte to read.
     *
     * file: [ ... ][ m_buffer ][ ... ]
     *               ^ m_originalBufferOffset
     *                  ^ m_offset (inside the buffer)
     *
     * file: [ ... ][ m_buffer ][ ... ]
     *               ^ m_originalBufferOffset
     *        ^ m_offset (before the buffer)
     *
     * file: [ ... ][ m_buffer ][ ... ]
     *               ^ m_originalBufferOffset
     *                             ^ m_offset (after the buffer)
     *
     * Invariants:
     *  - m_buffer holds exactly the file contents starting at m_originalBufferOffset.
     *  - tell() == m_offset, and m_offset is always the absolute logical next byte.
     *  - m_originalBufferOffset does not move when m_offset leaves the cached range.
     *  - Reading from m_buffer never moves the underlying file position.
     *  - Before a refill or direct read, the underlying position is brought to m_offset if it is not there.
     *  - Reads that bypass the buffer (remaining request >= m_maxBufferSize) retain it for later seeks back.
     *  - For buffer-only readers, m_originalBufferOffset is zero and m_buffer contains the whole file.
     */
    size_t m_originalBufferOffset{ 0 };
    AlignedBuffer m_buffer;
    size_t m_offset{ 0 };
    bool m_closed{ false };
};
}  // namespace rapidgzip
