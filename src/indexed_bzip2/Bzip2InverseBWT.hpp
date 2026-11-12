/**
 * @file Inverse Burrows-Wheeler transform for bzip2 blocks.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <core/common.hpp>

#include "Bzip2Format.hpp"

#if defined( __GNUC__ ) || defined( __clang__ )
    #define LIBRAPIDARCHIVE_BZIP2_PREFETCH( address ) __builtin_prefetch( ( address ), 0, 1 )
#elif defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_IX86 ) )
    #include <xmmintrin.h>
    #define LIBRAPIDARCHIVE_BZIP2_PREFETCH( address ) \
            _mm_prefetch( reinterpret_cast<const char*>( address ), _MM_HINT_T1 )
#else
    #define LIBRAPIDARCHIVE_BZIP2_PREFETCH( address )
#endif


namespace bzip2
{
/**
 * Inverts the BWT of a bzip2 block by following its LF mapping.
 *
 * The LF mapping leads from a row of the sorted rotation matrix to the row of the rotation that starts one
 * byte earlier, so following it from the row of the original string yields the string backwards:
 *
 * @verbatim
 * row = origPtr
 * for i = n - 1 ... 0:
 *     ( byte, rank ) = lfMapping[row]
 *     text[i] = byte
 *     row = cumulativeCounts[byte] + rank
 * @endverbatim
 *
 * Each step is a load from a random position of a table that is several megabytes large for a full block and
 * that depends on the previous load, so one such chain spends almost all of its time waiting for the cache.
 * The stream only stores the one start row, though, which is why the following trick is used to walk the same
 * cycle at many positions at once:
 *
 *  - Rows at regular distances are marked as chain starts. Their positions in the text are not known but they
 *    are spread over it more or less randomly.
 *  - A fixed number of lanes follow one chain each, interleaved step by step and with a software prefetch for
 *    the next step, so that the cache misses of all lanes overlap. A lane writes its bytes into its own
 *    contiguous buffer. All lanes share the write offset, so the address is the lane's constant offset plus one
 *    common register.
 *  - A chain ends when it arrives at a marked row, i.e., at the start of the chain that precedes it in the
 *    text. The lane notes the successor and continues with the next unstarted chain. Lanes that find no chain
 *    anymore keep stepping on a dummy row that maps to itself, which avoids any check in the common path.
 *  - In the end, the chains are concatenated in the order of their successors starting from the chain of the
 *    original row.
 *
 * Each table entry is still only visited once, so this does the same work as the single chain and nothing that
 * depends on the content is assumed about the marks: unfavorable inputs only make it as slow as one chain.
 */
class InverseBWT
{
private:
    /* Entries only use the lowest 28 bits: 20 for the rank and 8 for the byte. */
    static constexpr uint32_t CHAIN_START = uint32_t( 1 ) << 31U;
    static constexpr uint32_t NO_CHAIN = ~uint32_t( 0 );

    /* Small blocks fit into the caches, and the fixed costs of the lanes are not worth it for them. */
    static constexpr uint32_t MIN_SIZE_FOR_LANES = 4096;
    static constexpr uint32_t MAX_SIZE_FOR_FEW_LANES = 128 * 1024;
    /* Enough lanes to cover the memory latency. More lanes need more registers than x86-64 has. */
    static constexpr unsigned int FEW_LANES = 8;
    static constexpr unsigned int MANY_LANES = 24;
    /* A lane needs size / lanes bytes plus the time that the last chains take alone. The margin suffices
     * for all inputs that are not constructed to defeat the marks. The others use the overflow buffer. */
    static constexpr size_t FEW_LANES_STRIDE = ( MAX_SIZE_FOR_FEW_LANES + MAX_SIZE_FOR_FEW_LANES / 4 ) / FEW_LANES + 64;
    static constexpr size_t MANY_LANES_STRIDE = ( MAX_BLOCK_SIZE + MAX_BLOCK_SIZE / 4 ) / MANY_LANES + 64;

public:
    /** Size of the scratch buffer for @ref decode. */
    static constexpr size_t ARENA_SIZE = std::max( FEW_LANES * FEW_LANES_STRIDE, MANY_LANES * MANY_LANES_STRIDE );

public:
    /**
     * @param lfMapping One entry per byte of the BWT string: the byte in the lowest eight bits and above it
     *                  the number of earlier occurrences of this byte. It must have room for one more entry.
     *                  The chain start rows are overwritten, so it cannot be used again afterwards.
     * @param cumulativeCounts For each byte value the number of smaller bytes in the BWT string.
     * @param size Size of the BWT string, at most MAX_BLOCK_SIZE.
     * @param origPtr Row of the original string, must be smaller than @p size.
     * @param arena Scratch buffer of ARENA_SIZE bytes.
     * @param text Receives @p size bytes.
     */
    void
    decode( uint32_t* const                  lfMapping,
            const std::array<uint32_t, 256>& cumulativeCounts,
            const uint32_t                   size,
            const uint32_t                   origPtr,
            uint8_t* const                   arena,
            uint8_t* const                   text )
    {
        if ( size == 0 ) {
            return;
        }
        if ( ( size > MAX_BLOCK_SIZE ) || ( origPtr >= size ) ) {
            throw std::invalid_argument( "Invalid arguments for the inverse Burrows-Wheeler transform!" );
        }

        if ( size < MIN_SIZE_FOR_LANES ) {
            decodeSingleChain( lfMapping, cumulativeCounts.data(), size, origPtr, text );
        } else if ( size <= MAX_SIZE_FOR_FEW_LANES ) {
            decodeLanes<FEW_LANES, FEW_LANES_STRIDE>( lfMapping, cumulativeCounts.data(), size, origPtr,
                                                      arena, text );
        } else {
            decodeLanes<MANY_LANES, MANY_LANES_STRIDE>( lfMapping, cumulativeCounts.data(), size, origPtr,
                                                        arena, text );
        }
    }

private:
    struct Chain
    {
        /** The entry of the marked row, which has been replaced with CHAIN_START | chain index. */
        uint32_t firstEntry{ 0 };
        /** The chain that this one ran into, i.e., the one that precedes it in the text. */
        uint32_t successor{ 0 };
        uint32_t length{ 0 };
        uint32_t overflowLength{ 0 };
        const uint8_t* data{ nullptr };
        const uint8_t* overflowData{ nullptr };
        /** Offset inside its lane at which the first byte, the last one in text order, has been written.
         *  @ref data then points to the last byte written, which is the first one in text order. */
        ptrdiff_t startIndex{ 0 };
    };

private:
    /**
     * The text is periodic if following the mapping returns to the original row early. bzip2 defines the
     * output as @p size steps on that cycle, so the period is repeated.
     */
    static void
    repeatPeriod( uint8_t* const text,
                  const uint32_t period,
                  const uint32_t size )
    {
        for ( size_t filled = period; filled < size; ) {
            const auto count = std::min<size_t>( filled, size - filled );
            std::memcpy( text + filled, text, count );
            filled += count;
        }
    }

    static void
    decodeSingleChain( const uint32_t* const lfMapping,
                       const uint32_t* const cumulativeCounts,
                       const uint32_t        size,
                       const uint32_t        origPtr,
                       uint8_t* const        text )
    {
        uint32_t row = origPtr;
        uint32_t position = size;
        do {
            const auto entry = lfMapping[row];
            text[--position] = static_cast<uint8_t>( entry );
            row = cumulativeCounts[entry & 0xFFU] + ( entry >> 8U );
        } while ( ( position > 0 ) && ( row != origPtr ) );

        if ( position > 0 ) {
            const auto period = size - position;
            std::memmove( text, text + position, period );
            repeatPeriod( text, period, size );
        }
    }

    template<unsigned int LANES,
             size_t   STRIDE>
    void
    decodeLanes( uint32_t* const       lfMapping,
                 const uint32_t* const cumulativeCounts,
                 const uint32_t        size,
                 const uint32_t        origPtr,
                 uint8_t* const        arena,
                 uint8_t* const        text )
    {
        /* Mark the chain starts. Chain 0 starts at origPtr and therefore ends the text. The distance of about
         * sqrt( size ) / 2 rows balances the cost of starting a chain against the time that the last chains take
         * when there are fewer of them left than lanes, which grows with the chain length. */
        const auto distance = std::max<uint32_t>(
            16, static_cast<uint32_t>( std::sqrt( static_cast<double>( size ) ) ) / 2U );
        m_chains.clear();
        m_chains.reserve( size / distance + 2 );
        const auto markRow =
            [this, lfMapping] ( const uint32_t row )
            {
                Chain chain;
                chain.firstEntry = lfMapping[row];
                lfMapping[row] = CHAIN_START | static_cast<uint32_t>( m_chains.size() );
                m_chains.push_back( chain );
            };
        markRow( origPtr );
        for ( uint32_t row = distance / 2; row < size; row += distance ) {
            if ( row != origPtr ) {
                markRow( row );
            }
        }
        auto* const chains = m_chains.data();
        const auto chainCount = static_cast<uint32_t>( m_chains.size() );

        /* A lane without a chain idles on this row behind the table, which maps to itself. */
        const uint32_t idleRow = size;
        const uint32_t idleEntry = size << 8U;
        lfMapping[idleRow] = idleEntry;

        uint32_t rows[LANES];
        uint32_t laneChains[LANES];
        uint32_t nextChain = 0;
        uint32_t activeLanes = 0;
        /* All lanes write at the same offset and from the back to the front of their buffers so that
         * each chain is stored in text order. */
        ptrdiff_t index = static_cast<ptrdiff_t>( STRIDE ) - 1;

        for ( unsigned int lane = 0; lane < LANES; ++lane ) {
            uint32_t entry = idleEntry;
            laneChains[lane] = NO_CHAIN;
            if ( nextChain < chainCount ) {
                laneChains[lane] = nextChain;
                chains[nextChain].startIndex = index;
                entry = chains[nextChain].firstEntry;
                ++nextChain;
                ++activeLanes;
            }
            arena[lane * STRIDE + static_cast<size_t>( index )] = static_cast<uint8_t>( entry );
            rows[lane] = cumulativeCounts[entry & 0xFFU] + ( entry >> 8U );
        }
        --index;

    /* The body of the hot loop. The branch is only taken when a chain ends. */
    #define LIBRAPIDARCHIVE_BZIP2_LANE_STEP( LANE ) \
            { \
                uint32_t entry_ = lfMapping[rows[LANE]]; \
                if ( UNLIKELY( ( entry_ & CHAIN_START ) != 0 ) ) [[unlikely]] { \
                    auto& finished_ = chains[laneChains[LANE]]; \
                    finished_.successor = entry_ & ~CHAIN_START; \
                    finished_.length = static_cast<uint32_t>( finished_.startIndex - index ); \
                    finished_.data = arena + STRIDE * ( LANE ) + static_cast<size_t>( index + 1 ); \
                    if ( nextChain < chainCount ) { \
                        laneChains[LANE] = nextChain; \
                        chains[nextChain].startIndex = index; \
                        entry_ = chains[nextChain].firstEntry; \
                        ++nextChain; \
                    } else { \
                        laneChains[LANE] = NO_CHAIN; \
                        entry_ = idleEntry; \
                        if ( --activeLanes == 0 ) { \
                            goto allChainsFinished; \
                        } \
                    } \
                } \
                arena[STRIDE * ( LANE ) + static_cast<size_t>( index )] = static_cast<uint8_t>( entry_ ); \
                const uint32_t nextRow_ = cumulativeCounts[entry_ & 0xFFU] + ( entry_ >> 8U ); \
                rows[LANE] = nextRow_; \
                LIBRAPIDARCHIVE_BZIP2_PREFETCH( lfMapping + nextRow_ ); \
            }

    #define LIBRAPIDARCHIVE_BZIP2_LANE_STEPS_8( FIRST ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 0 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 1 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 2 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 3 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 4 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 5 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 6 ) \
            LIBRAPIDARCHIVE_BZIP2_LANE_STEP( FIRST + 7 )

        static_assert( ( LANES == 8 ) || ( LANES == 24 ), "The steps below are written out for these counts." );
        for ( ; index >= 0; --index ) {
            LIBRAPIDARCHIVE_BZIP2_LANE_STEPS_8( 0 )
            if constexpr ( LANES > 8 ) {
                LIBRAPIDARCHIVE_BZIP2_LANE_STEPS_8( 8 )
                LIBRAPIDARCHIVE_BZIP2_LANE_STEPS_8( 16 )
            }
        }

#undef LIBRAPIDARCHIVE_BZIP2_LANE_STEPS_8
#undef LIBRAPIDARCHIVE_BZIP2_LANE_STEP

        /* The lanes are full, which only happens when few chains are much longer than the others. Finish the
         * chains in the lanes and those that have not been started one after the other. Each entry of the
         * table is visited at most once, so everything fits into a buffer as large as the text. */
        {
            if ( m_overflow.size() < static_cast<size_t>( size ) ) {
                m_overflow.resize( size );
            }
            auto* target = m_overflow.data() + m_overflow.size();
            const auto followChain =
                [&] ( Chain&   chain,
                      uint32_t entry )
                {
                    const auto* const end = target;
                    while ( ( entry & CHAIN_START ) == 0 ) {
                        if ( target == m_overflow.data() ) {
                            throw std::logic_error( "The inverse BWT visited more rows than there are!" );
                        }
                        *--target = static_cast<uint8_t>( entry );
                        entry = lfMapping[cumulativeCounts[entry & 0xFFU] + ( entry >> 8U )];
                    }
                    chain.successor = entry & ~CHAIN_START;
                    chain.overflowData = target;
                    chain.overflowLength = static_cast<uint32_t>( end - target );
                };

            for ( unsigned int lane = 0; lane < LANES; ++lane ) {
                if ( laneChains[lane] == NO_CHAIN ) {
                    continue;
                }
                auto& chain = chains[laneChains[lane]];
                chain.length = static_cast<uint32_t>( chain.startIndex + 1 );
                chain.data = arena + lane * STRIDE;
                followChain( chain, lfMapping[rows[lane]] );
            }
            for ( ; nextChain < chainCount; ++nextChain ) {
                followChain( chains[nextChain], chains[nextChain].firstEntry );
            }
        }

allChainsFinished:
        /* Chains that are not on the cycle of the original row are not part of the text. */
        m_order.clear();
        size_t period = 0;
        uint32_t chainIndex = 0;
        do {
            m_order.push_back( chainIndex );
            period += static_cast<size_t>( chains[chainIndex].length ) + chains[chainIndex].overflowLength;
            chainIndex = chains[chainIndex].successor;
        } while ( ( chainIndex != 0 ) && ( chainIndex < chainCount ) && ( m_order.size() <= chainCount ) );
        if ( ( chainIndex != 0 ) || ( period == 0 ) || ( period > size ) ) {
            throw std::logic_error( "The chains of the inverse BWT do not form a cycle!" );
        }

        /* The first chain ends the text and each successor precedes its predecessor. Inside a chain, the part
         * in the overflow buffer has been decoded last and therefore comes first. Chains that have never been
         * started in a lane have no lane part and a null data pointer, which memcpy must not get. */
        auto position = period;
        for ( const auto id : m_order ) {
            const auto& chain = chains[id];
            if ( chain.length > 0 ) {
                position -= chain.length;
                std::memcpy( text + position, chain.data, chain.length );
            }
            if ( chain.overflowLength > 0 ) {
                position -= chain.overflowLength;
                std::memcpy( text + position, chain.overflowData, chain.overflowLength );
            }
        }
        repeatPeriod( text, static_cast<uint32_t>( period ), size );
    }

private:
    std::vector<Chain> m_chains;
    std::vector<uint32_t> m_order;
    std::vector<uint8_t> m_overflow;
};
}  // namespace bzip2
