/**
 * @file The hot loop that decodes the Huffman-coded symbols of a bzip2 block into run records.
 *
 * This file intentionally has no include guard and no namespace. Bzip2SymbolDecoder.hpp includes it once per
 * kernel variant from inside namespace bzip2 after defining:
 *
 *  - LIBRAPIDARCHIVE_BZIP2_KERNEL_NAME: the name of the function to define.
 *  - LIBRAPIDARCHIVE_BZIP2_KERNEL_TARGET: a function target attribute or nothing.
 *  - LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT: 0 for portable C++, 1 for SSE4.1, 2 for AVX2.
 *
 * Compiling the same loop several times with different target attributes is what makes runtime dispatch
 * possible without building the whole translation unit for a newer instruction set. A template cannot do this:
 * the target attribute applies to a function, not to an instantiation, and GCC and Clang refuse to inline
 * intrinsics such as _mm256_shuffle_epi8 into a function that was not compiled for their instruction set.
 * The per-variant parts are therefore macros, which also keeps all state in the registers of one function.
 *
 * All variants keep the decoder state in local variables, i.e., registers, and only branch for rare events:
 *
 *  - The bit buffer is refilled unconditionally with one unaligned 64-bit load for every two symbols.
 *  - A run symbol only changes the length of the current record. It is handled with masks instead of a branch
 *    because the sequence of literals and run symbols is close to random for the branch predictor.
 *  - The SIMD variants hold the first 32 entries of the Move-to-Front list in registers. Moving one of those
 *    to the front is a table-driven byte shuffle with a latency of a few cycles that does not go through memory.
 *    Only deeper list positions take a branch and shift the rest of the list in memory. That loop runs over
 *    all chunks used by the block's alphabet and masks the bytes behind the accessed position so that its trip
 *    count does not depend on the position and does not mispredict.
 */

#if LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT == 2

    #define BZIP2_KERNEL_CHUNK_SIZE 32U

    #define BZIP2_KERNEL_LOAD_FRONT() \
            __m256i front = _mm256_load_si256( reinterpret_cast<const __m256i*>( mtf ) )

    #define BZIP2_KERNEL_STORE_FRONT() \
            _mm256_store_si256( reinterpret_cast<__m256i*>( mtf ), front )

    /* The second shuffle works on the swapped halves so that bytes can cross the 128-bit lanes. */
    #define BZIP2_KERNEL_FRONT_UPDATE( SYMBOL ) \
            do { \
                const auto* const row_ = SYMBOL_KERNEL_FRONT_SHUFFLES.data() + ( SYMBOL ) * 64U; \
                front = _mm256_or_si256( \
                    _mm256_shuffle_epi8( front, _mm256_load_si256( reinterpret_cast<const __m256i*>( row_ ) ) ), \
                    _mm256_shuffle_epi8( _mm256_permute4x64_epi64( front, 0x4E ), \
                                         _mm256_load_si256( reinterpret_cast<const __m256i*>( row_ + 32 ) ) ) ); \
            } while ( false )

    #define BZIP2_KERNEL_FRONT_BYTE() \
            static_cast<uint32_t>( _mm_cvtsi128_si32( _mm256_castsi256_si128( front ) ) )

    /* Shift list positions [0, POSITION) one towards the back and put BYTE at the front. The memory part of the
     * list starts behind the register part. Chunks entirely behind POSITION are rewritten unchanged. */
    #define BZIP2_KERNEL_MOVE_TAIL_TO_FRONT( POSITION, BYTE ) \
            do { \
                const auto position_ = _mm256_set1_epi8( static_cast<char>( static_cast<uint32_t>( POSITION ) - \
                                                                            128U ) ); \
                auto previous_ = front; \
                for ( uint32_t chunk_ = 1; chunk_ < chunkCount; ++chunk_ ) { \
                    auto* const data_ = reinterpret_cast<__m256i*>( mtf + 32U * chunk_ ); \
                    const auto current_ = _mm256_load_si256( data_ ); \
                    const auto keep_ = _mm256_cmpgt_epi8( \
                        _mm256_load_si256( reinterpret_cast<const __m256i*>( \
                                               SYMBOL_KERNEL_BIASED_POSITIONS.data() + 32U * chunk_ ) ), position_ ); \
                    const auto shifted_ = _mm256_alignr_epi8( \
                        current_, _mm256_permute2x128_si256( current_, previous_, 0x03 ), 15 ); \
                    _mm256_store_si256( data_, _mm256_blendv_epi8( shifted_, current_, keep_ ) ); \
                    previous_ = current_; \
                } \
                const auto byte_ = _mm256_castsi128_si256( \
                    _mm_slli_si128( _mm_cvtsi32_si128( static_cast<int>( BYTE ) ), 15 ) ); \
                front = _mm256_alignr_epi8( front, _mm256_permute2x128_si256( front, byte_, 0x02 ), 15 ); \
            } while ( false )

#elif LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT == 1

    #define BZIP2_KERNEL_CHUNK_SIZE 16U

    #define BZIP2_KERNEL_LOAD_FRONT() \
            __m128i front0 = _mm_load_si128( reinterpret_cast<const __m128i*>( mtf ) ); \
            __m128i front1 = _mm_load_si128( reinterpret_cast<const __m128i*>( mtf + 16 ) )

    #define BZIP2_KERNEL_STORE_FRONT() \
            _mm_store_si128( reinterpret_cast<__m128i*>( mtf ), front0 ); \
            _mm_store_si128( reinterpret_cast<__m128i*>( mtf + 16 ), front1 )

    #define BZIP2_KERNEL_FRONT_UPDATE( SYMBOL ) \
            do { \
                const auto* const row_ = SYMBOL_KERNEL_FRONT_SHUFFLES.data() + ( SYMBOL ) * 64U; \
                const auto new0_ = _mm_or_si128( \
                    _mm_shuffle_epi8( front0, _mm_load_si128( reinterpret_cast<const __m128i*>( row_ ) ) ), \
                    _mm_shuffle_epi8( front1, _mm_load_si128( reinterpret_cast<const __m128i*>( row_ + 32 ) ) ) ); \
                front1 = _mm_or_si128( \
                    _mm_shuffle_epi8( front1, _mm_load_si128( reinterpret_cast<const __m128i*>( row_ + 16 ) ) ), \
                    _mm_shuffle_epi8( front0, _mm_load_si128( reinterpret_cast<const __m128i*>( row_ + 48 ) ) ) ); \
                front0 = new0_; \
            } while ( false )

    #define BZIP2_KERNEL_FRONT_BYTE() static_cast<uint32_t>( _mm_cvtsi128_si32( front0 ) )

    #define BZIP2_KERNEL_MOVE_TAIL_TO_FRONT( POSITION, BYTE ) \
            do { \
                const auto position_ = _mm_set1_epi8( static_cast<char>( static_cast<uint32_t>( POSITION ) - 128U ) ); \
                auto previous_ = front1; \
                for ( uint32_t chunk_ = 2; chunk_ < chunkCount; ++chunk_ ) { \
                    auto* const data_ = reinterpret_cast<__m128i*>( mtf + 16U * chunk_ ); \
                    const auto current_ = _mm_load_si128( data_ ); \
                    const auto keep_ = _mm_cmpgt_epi8( \
                        _mm_load_si128( reinterpret_cast<const __m128i*>( \
                                            SYMBOL_KERNEL_BIASED_POSITIONS.data() + 16U * chunk_ ) ), position_ ); \
                    _mm_store_si128( data_, _mm_blendv_epi8( _mm_alignr_epi8( current_, previous_, 15 ), \
                                                             current_, keep_ ) ); \
                    previous_ = current_; \
                } \
                front1 = _mm_alignr_epi8( front1, front0, 15 ); \
                front0 = _mm_or_si128( _mm_slli_si128( front0, 1 ), _mm_cvtsi32_si128( static_cast<int>( BYTE ) ) ); \
            } while ( false )

#endif

#if LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT != 0
    #define BZIP2_KERNEL_FRONT_SYMBOLS SIMD_KERNEL_FRONT_SIZE
#endif


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

#if LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT != 0
    auto* const mtf = reinterpret_cast<uint8_t*>( state.mtf.data() );
    /* Number of vector-sized chunks of the list that the alphabet of this block uses. */
    const uint32_t chunkCount = ( state.symbolCount + BZIP2_KERNEL_CHUNK_SIZE - 1U ) / BZIP2_KERNEL_CHUNK_SIZE;
    BZIP2_KERNEL_LOAD_FRONT();
#endif

/* A record is limited to 255 bytes. Longer runs are split into several records for the same byte.
 * splitLongRecord gets copies because taking the address of the hot loop variables would keep them in memory. */
#define BZIP2_KERNEL_CHECK_RECORD_LENGTH() \
        do { \
            if ( UNLIKELY( length > 255U ) ) [[unlikely]] { \
                auto splitRecord_ = record; \
                auto splitLength_ = length; \
                if ( !splitLongRecord( splitRecord_, recordLimit, splitLength_ ) ) { \
                    goto blockTooLarge; \
                } \
                record = splitRecord_; \
                length = splitLength_; \
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

#if LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT == 0

/* The portable variant branches between run symbols and literals and uses the packed list in memory. */
#define BZIP2_KERNEL_DECODE_SYMBOL() \
        do { \
            uint32_t entry_ = table->lookup( bitBuffer ); \
            if ( UNLIKELY( ( entry_ & SymbolTable::LENGTH_MASK ) == 0 ) ) [[unlikely]] { \
                entry_ = table->decodeLong( bitBuffer ); \
                if ( entry_ == SymbolTable::INVALID_ENTRY ) { \
                    goto invalidCode; \
                } \
            } \
            bitBuffer <<= ( entry_ & SymbolTable::LENGTH_MASK ); \
            bitCount -= entry_ & SymbolTable::LENGTH_MASK; \
            const uint32_t symbol_ = entry_ >> SymbolTable::LENGTH_BITS; \
            if ( symbol_ <= SYMBOL_RUNB ) { \
                length += runWeight << symbol_; \
                runWeight <<= 1U; \
                record[1] = static_cast<uint8_t>( length ); \
                BZIP2_KERNEL_CHECK_RECORD_LENGTH(); \
            } else if ( LIKELY( symbol_ != SymbolTable::END_OF_BLOCK ) ) [[likely]] { \
                const auto byte_ = moveToFront( state.mtf, symbol_ - 1U ); \
                BZIP2_KERNEL_START_RECORD( byte_ ); \
            } else { \
                goto endOfBlock; \
            } \
        } while ( false )

#else

/* Handles RUNA, RUNB, and literals at list positions below BZIP2_KERNEL_FRONT_SYMBOLS without a branch:
 *  - The list update is the identity for RUNA and RUNB, so it is done for all symbols.
 *  - runMask_ is all ones for RUNA (0) and RUNB (1) and zero for literals. It relies on the arithmetic right
 *    shift of negative numbers, which GCC, Clang, and MSVC implement and which C++20 guarantees.
 *  - A run symbol adds one or two times the run weight to the current record and doubles the weight. Multiplying
 *    with SYMBOL + 1 instead of shifting keeps the shift count register free for the bit buffer.
 *  - A literal starts a new record with length one. The record pointer only advances for literals, so a run
 *    symbol rewrites the current record. */
#define BZIP2_KERNEL_FRONT_SYMBOL( SYMBOL ) \
        do { \
            BZIP2_KERNEL_FRONT_UPDATE( SYMBOL ); \
            const uint32_t byte_ = BZIP2_KERNEL_FRONT_BYTE(); \
            const uint32_t runMask_ = static_cast<uint32_t>( static_cast<int32_t>( static_cast<uint32_t>( SYMBOL ) - \
                                                                                   2U ) >> 31U ); \
            const uint32_t isLiteral_ = runMask_ + 1U; \
            length = ( ( length + runWeight * ( static_cast<uint32_t>( SYMBOL ) + 1U ) ) & runMask_ ) + isLiteral_; \
            runWeight = ( ( runWeight + runWeight ) & runMask_ ) + isLiteral_; \
            record += RUN_RECORD_SIZE * isLiteral_; \
            record[0] = static_cast<uint8_t>( byte_ ); \
            record[1] = static_cast<uint8_t>( length ); \
            BZIP2_KERNEL_CHECK_RECORD_LENGTH(); \
        } while ( false )

/* Codes that are too long for the lookup table, deep list positions, and the end of the block all have an
 * entry above the limit so that the common case only needs this one branch. */
#define BZIP2_KERNEL_DECODE_SYMBOL() \
        do { \
            uint32_t entry_ = table->lookup( bitBuffer ); \
            if ( UNLIKELY( entry_ > ( ( BZIP2_KERNEL_FRONT_SYMBOLS << SymbolTable::LENGTH_BITS ) \
                                  | SymbolTable::LENGTH_MASK ) ) ) [[unlikely]] { \
                if ( ( entry_ & SymbolTable::LENGTH_MASK ) == 0 ) { \
                    entry_ = table->decodeLong( bitBuffer ); \
                    if ( entry_ == SymbolTable::INVALID_ENTRY ) { \
                        goto invalidCode; \
                    } \
                } \
                bitBuffer <<= ( entry_ & SymbolTable::LENGTH_MASK ); \
                bitCount -= entry_ & SymbolTable::LENGTH_MASK; \
                const uint32_t symbol_ = entry_ >> SymbolTable::LENGTH_BITS; \
                if ( symbol_ == SymbolTable::END_OF_BLOCK ) { \
                    goto endOfBlock; \
                } \
                if ( symbol_ > BZIP2_KERNEL_FRONT_SYMBOLS ) { \
                    const uint32_t tailPosition_ = symbol_ - 1U; \
                    const uint32_t tailByte_ = mtf[tailPosition_]; \
                    BZIP2_KERNEL_MOVE_TAIL_TO_FRONT( tailPosition_, tailByte_ ); \
                    BZIP2_KERNEL_START_RECORD( tailByte_ ); \
                } else { \
                    BZIP2_KERNEL_FRONT_SYMBOL( symbol_ ); \
                } \
            } else { \
                bitBuffer <<= ( entry_ & SymbolTable::LENGTH_MASK ); \
                bitCount -= entry_ & SymbolTable::LENGTH_MASK; \
                const uint32_t symbol_ = entry_ >> SymbolTable::LENGTH_BITS; \
                BZIP2_KERNEL_FRONT_SYMBOL( symbol_ ); \
            } \
        } while ( false )

#endif

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
#if LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT != 0
    BZIP2_KERNEL_STORE_FRONT();
#endif
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
#if LIBRAPIDARCHIVE_BZIP2_KERNEL_VARIANT != 0
    #undef BZIP2_KERNEL_FRONT_SYMBOLS
    #undef BZIP2_KERNEL_CHUNK_SIZE
    #undef BZIP2_KERNEL_LOAD_FRONT
    #undef BZIP2_KERNEL_STORE_FRONT
    #undef BZIP2_KERNEL_FRONT_UPDATE
    #undef BZIP2_KERNEL_FRONT_BYTE
    #undef BZIP2_KERNEL_MOVE_TAIL_TO_FRONT
    #undef BZIP2_KERNEL_FRONT_SYMBOL
#endif
