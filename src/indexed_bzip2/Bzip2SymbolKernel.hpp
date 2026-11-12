/**
 * @file The hot loop that decodes the Huffman-coded symbols of a bzip2 block into run records.
 *
 * This file intentionally has no include guard and no namespace. Bzip2SymbolDecoder.hpp includes it from inside
 * namespace bzip2 after defining:
 *
 *  - LIBRAPIDARCHIVE_BZIP2_KERNEL_NAME: the name of the function to define.
 *  - LIBRAPIDARCHIVE_BZIP2_KERNEL_TARGET: a function target attribute or nothing.
 *  - LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT: 0 for portable C++.
 *
 * The decoder state stays in local variables, i.e., registers, and only branches for rare events. The bit
 * buffer is refilled unconditionally with one unaligned 64-bit load for every two symbols. A run symbol only
 * changes the length of the current record and is handled with masks instead of a branch because the sequence
 * of literals and run symbols is close to random for the branch predictor.
 *
 * The portable variant keeps the Move-to-Front list in its packed in-memory form. The SIMD variants that keep
 * the first list entries in registers are added in a later commit.
 */

[[nodiscard]] LIBRAPIDARCHIVE_BZIP2_KERNEL_TARGET inline SymbolDecoderStatus
LIBRAPIDARCHIVE_BZIP2_KERNEL_NAME( SymbolDecoderState&  state,
                                   const uint8_t* const buffer,
                                   const size_t         bufferSize,
                                   size_t&              bitOffset ) noexcept
{
    /* A group of GROUP_SIZE symbols is decoded without any bounds checks on the input. */
    const auto byteOffset = bitOffset >> 3U;
    if ( ( bufferSize < SYMBOL_KERNEL_MINIMUM_INPUT ) || ( byteOffset > bufferSize - SYMBOL_KERNEL_MINIMUM_INPUT ) ) {
        return SymbolDecoderStatus::NEED_INPUT;
    }

    /* The next unread bit is the highest bit of bitBuffer, bitCount many bits are valid, and the bits behind
     * those are a preview of the byte that @p in points to. See Fabian Giesen's "Reading bits in far too many
     * ways", variant 4. */
    const uint8_t* in = buffer + byteOffset;
    const uint8_t* const inLimit = buffer + ( bufferSize - SYMBOL_KERNEL_MINIMUM_INPUT );
    uint64_t bitBuffer = 0;
    uint32_t bitCount = 0;

#define BZIP2_KERNEL_REFILL() \
        do { \
            bitBuffer |= loadBigEndian64( in ) >> bitCount; \
            in += ( 63U - bitCount ) >> 3U; \
            bitCount |= 56U; \
        } while ( false )

    BZIP2_KERNEL_REFILL();
    bitBuffer <<= ( bitOffset & 7U );
    bitCount -= static_cast<uint32_t>( bitOffset & 7U );

    uint8_t* LIBRAPIDARCHIVE_BZIP2_RESTRICT record = state.record;
    const uint8_t* const recordLimit = state.recordLimit;
    uint32_t length = state.length;
    uint32_t runWeight = state.runWeight;
    const uint8_t* selector = state.selector;
    const uint8_t* const selectorEnd = state.selectorEnd;
    const SymbolTable* const tables = state.tables;
    auto status = SymbolDecoderStatus::NEED_INPUT;

#define BZIP2_KERNEL_CHECK_RECORD_LENGTH() \
        do { \
            if ( UNLIKELY( length > 255U ) ) [[unlikely]] { \
                auto splitRecord = record; \
                auto splitLength = length; \
                if ( !splitLongRecord( splitRecord, recordLimit, splitLength ) ) { \
                    goto blockTooLarge; \
                } \
                record = splitRecord; \
                length = splitLength; \
            } \
        } while ( false )

#define BZIP2_KERNEL_START_RECORD( BYTE ) \
        do { \
            record += RUN_RECORD_SIZE; \
            record[0] = static_cast<uint8_t>( BYTE ); \
            record[1] = 1; \
            length = 1; \
            runWeight = 1; \
        } while ( false )


/* The portable variant branches between run symbols and literals and uses the packed list in memory. */
#define BZIP2_KERNEL_DECODE_SYMBOL() \
        do { \
            uint32_t entryValue = table->lookup( bitBuffer ); \
            if ( UNLIKELY( ( entryValue & SymbolTable::LENGTH_MASK ) == 0 ) ) [[unlikely]] { \
                entryValue = table->decodeLong( bitBuffer ); \
                if ( entryValue == SymbolTable::INVALID_ENTRY ) { \
                    goto invalidCode; \
                } \
            } \
            bitBuffer <<= ( entryValue & SymbolTable::LENGTH_MASK ); \
            bitCount -= entryValue & SymbolTable::LENGTH_MASK; \
            const uint32_t symbolValue = entryValue >> SymbolTable::LENGTH_BITS; \
            if ( symbolValue <= SYMBOL_RUNB ) { \
                length += runWeight << symbolValue; \
                runWeight <<= 1U; \
                record[1] = static_cast<uint8_t>( length ); \
                BZIP2_KERNEL_CHECK_RECORD_LENGTH(); \
            } else if ( LIKELY( symbolValue != SymbolTable::END_OF_BLOCK ) ) [[likely]] { \
                const auto byteValue = moveToFront( state.mtf, symbolValue - 1U ); \
                BZIP2_KERNEL_START_RECORD( byteValue ); \
            } else { \
                goto endOfBlock; \
            } \
        } while ( false )


    while ( true ) {
        if ( in > inLimit ) {
            break;
        }
        if ( UNLIKELY( selector == selectorEnd ) ) [[unlikely]] {
            status = SymbolDecoderStatus::OUT_OF_SELECTORS;
            break;
        }
        if ( UNLIKELY( record > recordLimit ) ) [[unlikely]] {
            goto blockTooLarge;
        }
        const SymbolTable* const table = tables + *selector++;

        /* One refill yields at least 56 valid bits, which suffices for two codes of MAX_HUFCODE_BITS = 20 bits.
         * The input checks happen once per group, which is why the buffer needs SYMBOL_KERNEL_MINIMUM_INPUT. */
        for ( int pair = 0; pair < GROUP_SIZE / 2; ++pair ) {
            BZIP2_KERNEL_REFILL();
            BZIP2_KERNEL_DECODE_SYMBOL();
            BZIP2_KERNEL_DECODE_SYMBOL();
        }
    }
    goto finished;

    /* Error and end states. The kernel always returns at a group boundary or at the end of the block, so the
     * state can be handed to decodeSymbolGroup or to the next call. */
endOfBlock:
    status = SymbolDecoderStatus::END_OF_BLOCK;
    goto finished;

invalidCode:
    status = SymbolDecoderStatus::INVALID_CODE;
    goto finished;

blockTooLarge:
    status = SymbolDecoderStatus::BLOCK_TOO_LARGE;

finished:
    state.record = record;
    state.length = length;
    state.runWeight = runWeight;
    state.selector = selector;
    bitOffset = static_cast<size_t>( in - buffer ) * 8U - bitCount;
    return status;
}


#undef BZIP2_KERNEL_REFILL
#undef BZIP2_KERNEL_CHECK_RECORD_LENGTH
#undef BZIP2_KERNEL_START_RECORD
#undef BZIP2_KERNEL_DECODE_SYMBOL
