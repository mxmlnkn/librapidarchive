#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bzip2
{
using CRC32LookupTable = std::array<uint32_t, 256>;


[[nodiscard]] constexpr CRC32LookupTable
createCRC32LookupTable() noexcept
{
    constexpr auto littleEndian{ false };
    CRC32LookupTable table{};
    for ( uint32_t i = 0; i < table.size(); ++i ) {
        uint32_t c = littleEndian ? i : i << 24U;
        for ( int j = 0; j < 8; ++j ) {
            if ( littleEndian ) {
                c = ( ( c & 1U ) != 0U ) ? ( c >> 1U ) ^ 0xEDB8'8320U : c >> 1U;
            } else {
                c = ( ( ( c & 0x8000'0000U ) != 0U ) ) ? ( c << 1U ) ^ 0x04C1'1DB7U : ( c << 1U );
            }
        }
        table[i] = c;
    }
    return table;
}


static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;

/* a small lookup table: raw data -> CRC32 value to speed up CRC calculation */
alignas( 64 ) constexpr static CRC32LookupTable CRC32_TABLE = createCRC32LookupTable();


[[nodiscard]] constexpr uint32_t
updateCRC32( uint32_t crc,
             uint8_t  data ) noexcept
{
    //return ( crc >> 8U ) ^ CRC32_TABLE[( crc ^ data ) & 0xFFU];
    return ( crc << 8U ) ^ CRC32_TABLE[( ( crc >> 24U ) ^ data ) & 0xFFU];
}


/**
 * Number of bytes processed per iteration by the bzip2 CRC. bzip2 uses the non-reflected (MSB-first)
 * CRC32 with polynomial 0x04C11DB7, so it cannot use the CRC32 CPU instruction and needs slicing tables.
 * Larger slices are faster in isolation but need more lookup-table cache lines.
 */
#ifndef LIBRAPIDARCHIVE_BZIP2_CRC_SLICES
    #define LIBRAPIDARCHIVE_BZIP2_CRC_SLICES 8
#endif

static constexpr unsigned int BZIP2_CRC_SLICES = LIBRAPIDARCHIVE_BZIP2_CRC_SLICES;
static_assert( ( BZIP2_CRC_SLICES == 1 ) || ( BZIP2_CRC_SLICES == 4 ) || ( BZIP2_CRC_SLICES == 8 ),
               "LIBRAPIDARCHIVE_BZIP2_CRC_SLICES must be 1, 4, or 8." );


/**
 * @param SLICE_SIZE number of bytes the table is used for.
 * @return tables where table[k][byte] is the CRC of @p byte followed by k zero bytes, so that table[0]
 *         is the ordinary byte-wise table.
 */
template<unsigned int SLICE_SIZE>
[[nodiscard]] constexpr std::array<CRC32LookupTable, SLICE_SIZE>
createCRC32SliceTables() noexcept
{
    std::array<CRC32LookupTable, SLICE_SIZE> tables{};
    tables[0] = createCRC32LookupTable();
    for ( unsigned int i = 1; i < SLICE_SIZE; ++i ) {
        for ( unsigned int byte = 0; byte < 256; ++byte ) {
            tables[i][byte] = ( tables[i - 1][byte] << 8U ) ^ tables[0][tables[i - 1][byte] >> 24U];
        }
    }
    return tables;
}


/**
 * Slicing-by-@p SLICE_SIZE update for the non-reflected bzip2 CRC32.
 *
 * For a slice of N data bytes d[0..N-1], the first four bytes are XORed with the four current CRC
 * bytes and looked up in the tables for the remaining N-1..N-4 zero bytes, while the later data
 * bytes only depend on the tables for the smaller remaining shifts. The final @c size % N bytes use
 * the byte-wise table as the tail.
 */
template<unsigned int SLICE_SIZE>
[[nodiscard]] uint32_t
updateCRC32SliceByN( uint32_t       crc,
                     const uint8_t* data,
                     size_t         size ) noexcept
{
    static_assert( ( SLICE_SIZE == 1 ) || ( SLICE_SIZE == 4 ) || ( SLICE_SIZE == 8 ),
                   "SLICE_SIZE must be 1, 4, or 8." );

    if constexpr ( SLICE_SIZE == 1 ) {
        for ( size_t i = 0; i < size; ++i ) {
            crc = updateCRC32( crc, data[i] );
        }
        return crc;
    } else {
        alignas( 64 ) static constexpr auto TABLES = createCRC32SliceTables<SLICE_SIZE>();

        size_t i = 0;
        for ( ; i + SLICE_SIZE <= size; i += SLICE_SIZE ) {
            uint32_t result = 0;
#if defined( __GNUC__ ) || defined( __clang__ )
            #pragma GCC unroll 8
#endif
            for ( unsigned int j = 0; j < SLICE_SIZE; ++j ) {
                const auto byte = data[i + j];
                const auto index = j < 4 ? static_cast<uint8_t>( ( crc >> ( 24U - 8U * j ) ) ^ byte ) : byte;
                result ^= TABLES[SLICE_SIZE - 1 - j][index];
            }
            crc = result;
        }

        for ( ; i < size; ++i ) {
            crc = updateCRC32( crc, data[i] );
        }
        return crc;
    }
}


[[nodiscard]] inline uint32_t
updateCRC32( uint32_t    crc,
             const char* data,
             size_t      size ) noexcept
{
    return updateCRC32SliceByN<BZIP2_CRC_SLICES>(
        crc, reinterpret_cast<const uint8_t*>( data ), size );
}
}  // namespace bzip2
