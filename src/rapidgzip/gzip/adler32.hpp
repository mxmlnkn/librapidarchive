#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>


namespace rapidgzip
{
/**
 * Computes the Adler-32 checksum used by the zlib container format.
 * @see https://www.ietf.org/rfc/rfc1950.txt
 */
class Adler32Calculator
{
public:
    void
    setEnabled( bool enabled ) noexcept
    {
        m_enabled = enabled;
    }

    [[nodiscard]] bool
    enabled() const noexcept
    {
        return m_enabled;
    }

    void
    reset() noexcept
    {
        m_s1 = 1;
        m_s2 = 0;
    }

    [[nodiscard]] uint32_t
    adler32() const noexcept
    {
        return static_cast<uint32_t>( ( m_s2 << 16U ) | m_s1 );
    }

    void
    update( const void* const data,
            size_t            size ) noexcept
    {
        if ( !m_enabled || ( size == 0 ) || ( data == nullptr ) ) {
            return;
        }

        const auto* bytes = static_cast<const uint8_t*>( data );
        while ( size > 0 ) {
            /* 5552 is the largest number of bytes for which the sums fit into 32 bits before reducing. */
            static constexpr size_t MAX_BYTES_BEFORE_MODULO = 5552;
            const auto nBytes = std::min( size, MAX_BYTES_BEFORE_MODULO );
            for ( size_t i = 0; i < nBytes; ++i ) {
                m_s1 += bytes[i];
                m_s2 += m_s1;
            }
            m_s1 %= MOD_ADLER;
            m_s2 %= MOD_ADLER;
            bytes += nBytes;
            size -= nBytes;
        }
    }

    /**
     * Throws on mismatch.
     */
    bool  // NOLINT(modernize-use-nodiscard)
    verify( const uint32_t adler32ToCompare ) const
    {
        if ( !enabled() || ( adler32() == adler32ToCompare ) ) {
            return true;
        }

        std::stringstream message;
        message << "Mismatching Adler32 (0x" << std::hex << adler32()
                << " <-> stored: 0x" << adler32ToCompare << ")!";
        throw std::domain_error( std::move( message ).str() );
    }

private:
    static constexpr uint32_t MOD_ADLER = 65521;

    bool m_enabled{ true };
    uint64_t m_s1{ 1 };
    uint64_t m_s2{ 0 };
};
}  // namespace rapidgzip
