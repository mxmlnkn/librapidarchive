#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <core/BitManipulation.hpp>
#include <huffman/HuffmanCodingSymbolsPerLength.hpp>


namespace rapidgzip
{
/**
 * This version uses a lookup table (LUT) to avoid repetitive loops over BitReader::read<1>() to speed things up a lot.
 * This started as a copy of @ref HuffmanCodingReversedBitsCached, however:
 * - It limits the LUT to a fixed size instead of caching everything up to MAX_CODE_LENGTH because that
 *   would be too large for bzip2, for which MAX_CODE_LENGTH is 20 instead of 16 for gzip.
 */
template<typename HuffmanCode,
         uint8_t  MAX_CODE_LENGTH,
         typename Symbol,
         size_t   MAX_SYMBOL_COUNT,
         uint8_t  LUT_BITS_COUNT,
         bool     REVERSE_BITS,
         bool     CHECK_OPTIMALITY = true>
class HuffmanCodingShortBitsCached :
    public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT, CHECK_OPTIMALITY>
{
public:
    using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT,
                                                   CHECK_OPTIMALITY>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

    /** Cache entries store the code length in the upper 16 bits and the symbol in the lower 16 bits. */
    static constexpr uint32_t ENTRY_LENGTH_SHIFT = 16;
    static constexpr uint32_t SYMBOL_MASK = 0xFFFFU;
    static constexpr uint32_t INVALID_SYMBOL = 0xFFFFFFFFU;

public:
    [[nodiscard]] constexpr Error
    initializeFromLengths( const VectorView<BitCount>& codeLengths )
    {
        if ( const auto errorCode = BaseType::initializeFromLengths( codeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        m_lutBitsCount = std::min( LUT_BITS_COUNT, this->m_maxCodeLength );
        m_bitsToReadAtOnce = std::max( LUT_BITS_COUNT, this->m_minCodeLength );

        /* Initialize the cache. */
        if ( m_needsToBeZeroed ) {
            // Works constexpr
            for ( size_t symbol = 0; symbol < m_codeCache.size(); ++symbol ) {
                m_codeCache[symbol] = 0;
            }
        }

        auto codeValues = this->m_minimumCodeValuesPerLevel;
        for ( size_t symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            /* Note that the preemptive continue for large lengths is not a bug even if we do not increment
             * codeValues because all symbols of the same length will either be filtered or not, so that
             * the missing increment does not matter because that code is either always used or never. */
            if ( ( length == 0 ) || ( length > m_lutBitsCount ) ) {
                continue;
            }

            const auto fillerBitCount = static_cast<uint8_t>( m_lutBitsCount - length );
            const auto k = length - this->m_minCodeLength;
            const auto code = codeValues[k]++;
            if constexpr ( REVERSE_BITS ) {
                const auto reversedCode = reverseBits( code, length );
                const auto maximumPaddedCode = static_cast<HuffmanCode>(
                    reversedCode | HuffmanCode( nLowestBitsSet<HuffmanCode>( fillerBitCount ) << length ) );
                assert( maximumPaddedCode < m_codeCache.size() );
                const auto increment = static_cast<HuffmanCode>( HuffmanCode( 1 ) << length );
                for ( auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode; paddedCode += increment ) {
                    m_codeCache[paddedCode] = ( static_cast<uint32_t>( length ) << ENTRY_LENGTH_SHIFT )
                                              | static_cast<uint32_t>( symbol );
                }
            } else {
                const auto maximumPaddedCode = static_cast<HuffmanCode>(
                    ( code << fillerBitCount ) | nLowestBitsSet<HuffmanCode>( fillerBitCount ) );
                assert( maximumPaddedCode < m_codeCache.size() );
                for ( auto paddedCode = code << fillerBitCount; paddedCode <= maximumPaddedCode; ++paddedCode ) {
                    m_codeCache[paddedCode] = ( static_cast<uint32_t>( length ) << ENTRY_LENGTH_SHIFT )
                                              | static_cast<uint32_t>( symbol );
                }
            }
        }

        m_needsToBeZeroed = true;

        return Error::NONE;
    }

    template<typename BitReader>
    [[nodiscard]] forceinline std::optional<Symbol>
    decode( BitReader& bitReader ) const
    {
        /* An all-zero alphabet has no symbols and m_lutBitsCount is zero in that case. Peeking zero bits would
         * assert in the BitReader. */
        if ( UNLIKELY( m_lutBitsCount == 0 ) ) [[unlikely]] {
            return std::nullopt;
        }

        try {
            /* Use the compile-time peek when the full LUT is in use so the index mask and size become
             * constants. m_lutBitsCount only differs from LUT_BITS_COUNT when the alphabet's maximum code
             * length is smaller than the LUT (then peeking LUT_BITS_COUNT bits may read past valid codes). */
            const auto index = LUT_BITS_COUNT == m_lutBitsCount
                               ? static_cast<HuffmanCode>( bitReader.template peek<LUT_BITS_COUNT>() )
                               : static_cast<HuffmanCode>( bitReader.peek( m_lutBitsCount ) );
            const auto entry = m_codeCache[index];
            const auto length = static_cast<BitCount>( entry >> ENTRY_LENGTH_SHIFT );
            if ( length == 0 ) {
                return decodeLong( bitReader );
            }
            bitReader.seekAfterPeek( length );
            return static_cast<Symbol>( entry & SYMBOL_MASK );
        } catch ( const EndOfFileReached& ) {
            /* Should only happen at the end of the file and probably not even there
             * because the bzip2 footer (EOS block) should be longer than the peek length. */
            return BaseType::decode( bitReader );
        }
    }

    /** Hot-path variant of @ref decode that returns @ref INVALID_SYMBOL instead of an @ref std::optional. */
    template<typename BitReader>
    [[nodiscard]] forceinline uint32_t
    decodePacked( BitReader& bitReader ) const
    {
        if ( UNLIKELY( m_lutBitsCount == 0 ) ) [[unlikely]] {
            return INVALID_SYMBOL;
        }

        try {
            const auto index = LUT_BITS_COUNT == m_lutBitsCount
                               ? static_cast<HuffmanCode>( bitReader.template peek<LUT_BITS_COUNT>() )
                               : static_cast<HuffmanCode>( bitReader.peek( m_lutBitsCount ) );
            const auto entry = m_codeCache[index];
            const auto length = entry >> ENTRY_LENGTH_SHIFT;
            if ( UNLIKELY( length == 0 ) ) [[unlikely]] {
                const auto result = decodeLong( bitReader );
                return result ? static_cast<uint32_t>( *result ) : INVALID_SYMBOL;
            }
            bitReader.seekAfterPeek( length );
            return entry & SYMBOL_MASK;
        } catch ( const EndOfFileReached& ) {
            const auto result = BaseType::decode( bitReader );
            return result ? static_cast<uint32_t>( *result ) : INVALID_SYMBOL;
        }
    }

private:
    template<typename BitReader>
    [[nodiscard]] forceinline constexpr std::optional<Symbol>
    decodeLong( BitReader& bitReader ) const
    {
        HuffmanCode code = 0;

        /** Read the first n bytes. Note that we can't call the bitReader with argument > 1 because the bit order
         * would be inversed. @todo Reverse the Huffman codes and prepend bits instead of appending, so that this
         * first step can be conflated and still have the correct order for comparison! */
        if constexpr ( REVERSE_BITS ) {
            for ( BitCount i = 0; i < m_bitsToReadAtOnce; ++i ) {
                code = HuffmanCode( code << 1U ) | bitReader.template read<1>();
            }
        } else {
            code = static_cast<HuffmanCode>( bitReader.read( m_bitsToReadAtOnce ) );
        }

        for ( BitCount k = m_bitsToReadAtOnce - this->m_minCodeLength;
              k <= this->m_maxCodeLength - this->m_minCodeLength; ++k )
        {
            const auto minCode = this->m_minimumCodeValuesPerLevel[k];
            if ( minCode <= code ) {
                const auto subIndex = this->m_offsets[k] + static_cast<size_t>( code - minCode );
                if ( subIndex < this->m_offsets[k + 1] ) {
                    return this->m_symbolsPerLength[subIndex];
                }
            }

            code <<= 1;
            code |= bitReader.template read<1>();
        }

        return std::nullopt;
    }

private:
    /**
     * Total m_codeCache sizes for varying LUT_BITS_COUNT:
     *  - 10 bits -> 4 KiB
     *  - 11 bits -> 8 KiB
     *  - 12 bits -> 16 KiB
     */
    alignas( 64 ) std::array<uint32_t, ( 1UL << LUT_BITS_COUNT )> m_codeCache{};

    uint8_t m_lutBitsCount{ LUT_BITS_COUNT };
    uint8_t m_bitsToReadAtOnce{ LUT_BITS_COUNT };
    bool m_needsToBeZeroed{ false };
};
}  // namespace rapidgzip
