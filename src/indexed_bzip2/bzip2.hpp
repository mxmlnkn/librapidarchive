/**
 * Modified version of bzcat.c part of toybox commit 7bf68329eb3b
 * by Rob Landley released under SPDX-0BSD license.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

/* ARM64EC also defines _M_X64 and _M_AMD64, but it is an ARM64 target and must not take the x64
 * SSE2 path. The vendored libsais uses the same condition. */
#if ( defined( __SSE2__ ) || ( defined( _M_IX86_FP ) && ( _M_IX86_FP >= 2 ) ) \
      || defined( _M_AMD64 ) || defined( _M_X64 ) ) && !defined( _M_ARM64EC )
    #define LIBRAPIDARCHIVE_BZIP2_HAS_SSE2 1
    #include <emmintrin.h>
#else
    #define LIBRAPIDARCHIVE_BZIP2_HAS_SSE2 0
#endif
#if defined( __ARM_NEON )
    #include <arm_neon.h>
#endif

#include <core/BitManipulation.hpp>
#include <core/VectorView.hpp>
#include <filereader/BitReader.hpp>
#include <huffman/HuffmanCodingShortBitsCached.hpp>
#include <libsais.h>

#include "Bzip2Crc32.hpp"

#if LIBRAPIDARCHIVE_BZIP2_CRC_PCLMUL
    /* The AVX-512 RLE scan uses the same compiler/architecture gate as the PCLMUL CRC header. */
    #define LIBRAPIDARCHIVE_BZIP2_AVX512_SCAN 1
#else
    #define LIBRAPIDARCHIVE_BZIP2_AVX512_SCAN 0
#endif


/**
 * @see https://github.com/dsnet/compress/blob/master/doc/bzip2-format.pdf
 * @verbatim
 * Symbol               Expression
 * ------------------------------------------
 * BZipFile             := BZipStream (one or more)
 * └──BZipStream        := StreamHeader StreamBlock* StreamFooter
 *    ├──StreamHeader   := HeaderMagic Version Level                          -> readBzip2Header
 *    ├──StreamBlock    := BlockHeader BlockTrees BlockData(Huffman encoded)
 *    │ ├──BlockHeader  := BlockMagic BlockCRC Randomized OrigPtr             -> readBlockHeader
 *    │ ├──BlockTrees   := SymMap NumTrees NumSels Selectors Trees            -> readTrees
 *    | |  ├──SymMap    := MapL1 MapL2{1,16}
 *    │ |  ├──Selectors := Selector{NumSels}
 *    │ |  └──Trees     := (BitLen Delta{NumSyms}){NumTrees}
 *    | └──BlockData    := Huffman-Encoded data
 *    └──StreamFooter   := FooterMagic StreamCRC Padding                      -> Block::eos
 * @endverbatim

 * 1. Run-length encoding (RLE) of initial data
 * 2. Burrows–Wheeler transform (BWT), or block sorting
 * 3. Move-to-front (MTF) transform
 * 4. Run-length encoding (RLE) of MTF result
 * 5. Huffman coding
 * 6. Selection between multiple Huffman tables
 * 7. Unary base-1 encoding of Huffman table selection
 * 8. Delta encoding (Δ) of Huffman-code bit lengths
 * 9. Sparse bit array showing which symbols are used
 */
namespace bzip2
{
/* Constants for huffman coding */
static constexpr uint32_t MAX_GROUPS = 6;
static constexpr int GROUP_SIZE = 50;       /* 64 would have been more efficient */
static constexpr int MAX_HUFCODE_BITS = 20; /* Longest huffman code allowed */
static constexpr size_t MAX_SYMBOLS = 258;  /* 256 literals + RUNA + RUNB */
static constexpr uint16_t SYMBOL_RUNA = 0;
static constexpr uint16_t SYMBOL_RUNB = 1;

/* bzip2 levels 1-9 use blocks of at most 100000 to 900000 bytes before the BWT. */
static constexpr size_t MAX_BLOCK_SIZE = 900000;


#if LIBRAPIDARCHIVE_BZIP2_AVX512_SCAN
#define LIBRAPIDARCHIVE_BZIP2_AVX512_TARGET __attribute__( ( target( "avx512f,avx512bw" ) ) )

[[nodiscard]] inline bool
hasAvx512RLEScan() noexcept
{
    static const bool supported =
        ( __builtin_cpu_supports( "avx512f" ) != 0 ) && ( __builtin_cpu_supports( "avx512bw" ) != 0 );
    return supported;
}


/**
 * Expand the whole RLE1 prefix of a block 64 positions at a time. A position starts a run when it and the next
 * three bytes are equal. The byte after those four is the additional repeat count. The first set bit of the
 * equality mask is therefore the first run start, and everything before it is a literal.
 *
 * Stops when the caller's buffer is full, the source is exhausted, or fewer than 68 source bytes remain, so
 * the scalar and SSE2 paths in decodeBlock can handle the tail.
 */
LIBRAPIDARCHIVE_BZIP2_AVX512_TARGET
inline void
expandRLEAvx512( const uint8_t* source,
                 const size_t   sourceSize,
                 size_t&        position,
                 char* const    output,
                 size_t&        nBytesDecoded,
                 const size_t   nMaxBytesToDecode,
                 uint8_t&       repeatByte,
                 size_t&        repeatCount )
{
    while ( nBytesDecoded < nMaxBytesToDecode ) {
        if ( repeatCount > 0 ) {
            const auto nBytesToRepeat = std::min( repeatCount, nMaxBytesToDecode - nBytesDecoded );
            for ( size_t i = 0; i < nBytesToRepeat; ++i ) {
                output[nBytesDecoded + i] = static_cast<char>( repeatByte );
            }
            nBytesDecoded += nBytesToRepeat;
            repeatCount -= nBytesToRepeat;
            continue;
        }

        if ( ( position >= sourceSize )
             || ( nBytesDecoded + sizeof( __m512i ) > nMaxBytesToDecode )
             || ( position + sizeof( __m512i ) + 4 > sourceSize ) ) {
            return;
        }

        const auto* const window = source + position;
        const auto v0 = _mm512_loadu_si512( window );
        const auto v1 = _mm512_loadu_si512( window + 1 );
        const auto v2 = _mm512_loadu_si512( window + 2 );
        const auto v3 = _mm512_loadu_si512( window + 3 );
        const auto runStarts = _mm512_cmpeq_epi8_mask( v0, v1 )
                               & _mm512_cmpeq_epi8_mask( v0, v2 )
                               & _mm512_cmpeq_epi8_mask( v0, v3 );
        if ( runStarts == 0 ) {
            _mm512_storeu_si512( output + nBytesDecoded, v0 );
            nBytesDecoded += sizeof( __m512i );
            position += sizeof( __m512i );
            continue;
        }

        const auto literalCount = static_cast<size_t>( __builtin_ctzll( runStarts ) );
        if ( literalCount > 0 ) {
            const auto literalMask = ( __mmask64( 1 ) << literalCount ) - 1;
            _mm512_mask_storeu_epi8( output + nBytesDecoded, literalMask, v0 );
        }
        nBytesDecoded += literalCount;
        position += literalCount;
        repeatByte = source[position];
        repeatCount = 4 + source[position + 4];
        position += 5;
    }
}
#undef LIBRAPIDARCHIVE_BZIP2_AVX512_TARGET
#endif


constexpr auto MAGIC_BITS_BLOCK = 0x314159265359ULL;  /* bcd(pi) */
constexpr auto MAGIC_BITS_EOS = 0x177245385090ULL;  /* bcd(sqrt(pi)) */
constexpr auto MAGIC_BITS_SIZE = 48;
constexpr std::string_view MAGIC_BYTES_BZ2 = "BZh";

using BitReader = rapidgzip::BitReader<true, uint64_t>;


/**
 * bzip2's Move-to-Front (MTF) list for the block data. Eight symbols are packed into one 64-bit word, with the
 * symbol at list position p stored in byte p % 8 of word p / 8. This avoids the variable-length memmove that
 * otherwise dominates the literal path for alphabets with a uniform MTF index distribution such as base64.
 */
using PackedMTF = std::array<uint64_t, 256 / 8>;


#if defined( __ARM_NEON )
/* Compares each of the first 64 list positions against the insert position. */
constexpr std::array<uint8_t, 64> MTF_INDEX_BYTES =
    [] ()
    {
        std::array<uint8_t, 64> result{};
        for ( size_t i = 0; i < result.size(); ++i ) {
            result[i] = static_cast<uint8_t>( i );
        }
        return result;
    }();
#endif


[[nodiscard]] constexpr PackedMTF
createPackedMTF() noexcept
{
    PackedMTF result{};
    for ( size_t word = 0; word < result.size(); ++word ) {
        uint64_t value = 0;
        for ( size_t byte = 0; byte < 8; ++byte ) {
            value |= static_cast<uint64_t>( word * 8 + byte ) << ( byte * 8 );
        }
        result[word] = value;
    }
    return result;
}


/**
 * bzip2's Move-to-Front (MTF) list is a recency-ordered cache of the 256 byte values.
 * Encoding a symbol writes its current list position and then moves it to the front.
 * For the input "bzipbzip" and the reduced alphabet "abc...z" the positions are:
 * 1, 25, 9, 16, 3, 3, 3, 3
 *
 * @verbatim
 *        |  SSE 128 Bit  |...
 *        |uint64 |uint64 |...
 *   b -> "a>b<c d|e f g h|..."     -> position  1, list becomes "bac..."
 *   z -> "b a c d|e f g h|...>z<"  -> position 25, list becomes "zbac..."
 *   i -> "z b a d|e f g h|...>i<"  -> position  9, list becomes "izba..."
 *   p -> "i z b a|d f g h|... p"   -> position 16, list becomes "pizba..."
 *   b -> "p i z>b<a f g h|... z"   -> position  3, list becomes "bpiza..."
 *   ...
 * @endverbatim
 *
 * After the first "bzip" the list starts with "p i z b a ...", so the second "bzip" finds every
 * symbol three positions behind the front. The list is therefore a diff against the recent symbol
 * order: the most recent symbol is always at position 0, the one before it at position 1, and so
 * on. Repeating a symbol yields a small index. The decoder only has to reverse the same update.
 *
 * The list is packed as 32 uint64 words with 8 symbols per word (see PackedMTF), so list position p
 * lives in byte p % 8 of word p / 8. A plain array MTF update would shift a variable-length prefix.
 * This function instead updates the first 64 positions unconditionally: it saves the
 * selected symbol, shifts the whole 64-byte prefix one position towards the back, puts the saved
 * symbol at the front, and keeps the suffix after the selected position unchanged. That is exactly
 * the list update, but without a data-dependent loop exit.
 *
 * The SSE2 path treats the 64 bytes as four 16-byte vectors. _mm_slli_si128 shifts every vector by
 * one byte towards higher list positions; _mm_srli_si128(previous, 15) moves the last byte of the
 * previous vector into the low byte of the next one, so the four operations form one continuous
 * 64-byte shift. limit = position + 1 and the index vectors 0..63 produce a 0xFF mask for the
 * positions <= position and 0x00 above it. The blend therefore selects the shifted prefix [0,
 * position] and the original suffix (position, 64), which is the same result as moving the selected
 * symbol to the front and shifting only the symbols in between. The saved symbol is inserted at
 * position 0 by _mm_cvtsi32_si128.
 *
 * The branchless path matters for alphabets with a uniform MTF index distribution such as base64
 * after BWT: the scalar loop exits after a data-dependent number of words on almost every literal,
 * which the branch predictor cannot learn. Shifting all 64 positions costs some extra work for small
 * indices but removes that misprediction. Positions >= 64 and targets without SSE2 use the scalar
 * word loop below, which propagates the carry byte through the packed words.
 *
 * An update at position p only changes the prefix [0, p]; positions above p are unchanged.
 * A following index q > p therefore reads an unchanged list element and could be gathered before the update.
 * This implementation still stores the full 64-byte prefix unconditionally, so the next load aliases that store.
 * Batching increasing MTF indices and gathering their symbols from the unchanged suffix is a more promising
 * optimization than widening the vectors: AVX2 and AVX-512 variants kept the same store/load dependency and
 * measured slower than this SSE2 version.
 *
 * @return the symbol byte that is now at the front of the list.
 */
[[nodiscard]] forceinline uint8_t
moveToFront( PackedMTF&   mtf,
             const size_t position ) noexcept
{
#if LIBRAPIDARCHIVE_BZIP2_HAS_SSE2
    if ( position < 64 ) {
        auto* const data = reinterpret_cast<uint8_t*>( mtf.data() );
        /* Keep the selected symbol because shifting overwrites its old list position. */
        const auto head = data[position];

        /* Load 64 B (four 16-byte vectors) out of the maximum 256 B and shift it unconditionally by 1 B
         * to make place at the front. */
        const auto in0 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( data ) );
        const auto in1 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( data + sizeof( __m128i ) ) );
        const auto in2 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( data + 2 * sizeof( __m128i ) ) );
        const auto in3 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( data + 3 * sizeof( __m128i ) ) );

        /* The high byte of one vector becomes the low byte of the next, so these four shifts form
         * one continuous 64-byte shift. The selected symbol fills the freed position 0. */
        auto out0 = _mm_slli_si128( in0, 1 );
        out0 = _mm_or_si128( out0, _mm_cvtsi32_si128( head ) );
        const auto out1 = _mm_or_si128( _mm_slli_si128( in1, 1 ), _mm_srli_si128( in0, sizeof( __m128i ) - 1 ) );
        const auto out2 = _mm_or_si128( _mm_slli_si128( in2, 1 ), _mm_srli_si128( in1, sizeof( __m128i ) - 1 ) );
        const auto out3 = _mm_or_si128( _mm_slli_si128( in3, 1 ), _mm_srli_si128( in2, sizeof( __m128i ) - 1 ) );

        /* The idea here is to vector-compare index <= position, where we build all indexes like std::iota to get
         * masks. 0xFF for positions <= position selects the shifted prefix; 0x00 keeps the suffix above. */
        const auto limit = _mm_set1_epi8( static_cast<char>( position + 1 ) );
        const auto index0 = _mm_setr_epi8( 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 );
        const auto index1 = _mm_setr_epi8( 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 );
        const auto index2 = _mm_setr_epi8( 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47 );
        const auto index3 = _mm_setr_epi8( 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 );

        const auto mask0 = _mm_cmpgt_epi8( limit, index0 );
        const auto mask1 = _mm_cmpgt_epi8( limit, index1 );
        const auto mask2 = _mm_cmpgt_epi8( limit, index2 );
        const auto mask3 = _mm_cmpgt_epi8( limit, index3 );

        /**
         * Now we have some mask = [mask0, mask1, mask2, mask3] = 1111..1 (@p position) 0..00000 that can be
         * used to merge the shifted version of the alphabet with the unshifted version after @p position.
         * Representation with 8 instead of 64 symbol/byte shifts:
         * @verbatim
         *         Symbol f at position 5 to be moved to front
         *                              v
         * Index            : 0 1 2 3 4 5 6 7
         * Alphabet         : a b c d e f g h
         * Alphabet >> 1    : _ a b c d e f g
         * Mask (idx <= p)  : 1 1 1 1 1 1 0 0
         * Mask & (Al >> 1) : _ a b c d e _ _
         * ~Mask & Alphabet : _ _ _ _ _ _ g h
         * OR               : _ a b c d e g h
         * Insert head      : f a b c d e g h
         * @endverbatim
         * Beware! The shift direction and exact position is confusing because of little endian storage order! SSE is
         * used to shift left by 1, so that the byte representation is shifted to the "right" in usual index order.
         */
        _mm_storeu_si128( reinterpret_cast<__m128i*>( data ),
                          _mm_or_si128( _mm_and_si128( mask0, out0 ),
                                        _mm_andnot_si128( mask0, in0 ) ) );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( data + 16 ),
                          _mm_or_si128( _mm_and_si128( mask1, out1 ),
                                        _mm_andnot_si128( mask1, in1 ) ) );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( data + 32 ),
                          _mm_or_si128( _mm_and_si128( mask2, out2 ),
                                        _mm_andnot_si128( mask2, in2 ) ) );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( data + 48 ),
                          _mm_or_si128( _mm_and_si128( mask3, out3 ),
                                        _mm_andnot_si128( mask3, in3 ) ) );
        return head;
    }

    /* Positions 64 to 255 previously used the scalar word loop below, which carries one byte through
     * up to 31 words. The packed list is 256 bytes, so the same 64-byte shift can be applied to whole
     * 64-byte chunks and only the chunk containing the selected position needs the prefix mask. */
    if ( position < mtf.size() * sizeof( uint64_t ) ) {
        auto* const data = reinterpret_cast<uint8_t*>( mtf.data() );
        const auto head = data[position];
        const auto fullChunks = position / 64;
        auto carry = head;

        const auto shiftFullChunk =
            [] ( uint8_t* const chunk, const uint8_t incomingByte ) noexcept
            {
                const auto in0 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk ) );
                const auto in1 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk + sizeof( __m128i ) ) );
                const auto in2 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk + 2 * sizeof( __m128i ) ) );
                const auto in3 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk + 3 * sizeof( __m128i ) ) );
                _mm_storeu_si128( reinterpret_cast<__m128i*>( chunk ),
                                  _mm_or_si128( _mm_slli_si128( in0, 1 ), _mm_cvtsi32_si128( incomingByte ) ) );
                _mm_storeu_si128(
                    reinterpret_cast<__m128i*>( chunk + sizeof( __m128i ) ),
                    _mm_or_si128( _mm_slli_si128( in1, 1 ), _mm_srli_si128( in0, sizeof( __m128i ) - 1 ) ) );
                _mm_storeu_si128(
                    reinterpret_cast<__m128i*>( chunk + 2 * sizeof( __m128i ) ),
                    _mm_or_si128( _mm_slli_si128( in2, 1 ), _mm_srli_si128( in1, sizeof( __m128i ) - 1 ) ) );
                _mm_storeu_si128(
                    reinterpret_cast<__m128i*>( chunk + 3 * sizeof( __m128i ) ),
                    _mm_or_si128( _mm_slli_si128( in3, 1 ), _mm_srli_si128( in2, sizeof( __m128i ) - 1 ) ) );
                return static_cast<uint8_t>(
                    _mm_cvtsi128_si32( _mm_srli_si128( in3, sizeof( __m128i ) - 1 ) ) );
            };

        for ( size_t chunk = 0; chunk < fullChunks; ++chunk ) {
            carry = shiftFullChunk( data + chunk * 64, carry );
        }

        /* The chunk containing the selected position is shifted up to and including that position. */
        auto* const chunk = data + fullChunks * 64;
        const auto in0 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk ) );
        const auto in1 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk + sizeof( __m128i ) ) );
        const auto in2 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk + 2 * sizeof( __m128i ) ) );
        const auto in3 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( chunk + 3 * sizeof( __m128i ) ) );
        auto out0 = _mm_slli_si128( in0, 1 );
        out0 = _mm_or_si128( out0, _mm_cvtsi32_si128( carry ) );
        const auto out1 = _mm_or_si128( _mm_slli_si128( in1, 1 ), _mm_srli_si128( in0, sizeof( __m128i ) - 1 ) );
        const auto out2 = _mm_or_si128( _mm_slli_si128( in2, 1 ), _mm_srli_si128( in1, sizeof( __m128i ) - 1 ) );
        const auto out3 = _mm_or_si128( _mm_slli_si128( in3, 1 ), _mm_srli_si128( in2, sizeof( __m128i ) - 1 ) );

        const auto limit = _mm_set1_epi8( static_cast<char>( ( position % 64 ) + 1 ) );
        const auto index0 = _mm_setr_epi8( 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 );
        const auto index1 = _mm_setr_epi8( 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 );
        const auto index2 = _mm_setr_epi8( 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47 );
        const auto index3 = _mm_setr_epi8( 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 );
        const auto mask0 = _mm_cmpgt_epi8( limit, index0 );
        const auto mask1 = _mm_cmpgt_epi8( limit, index1 );
        const auto mask2 = _mm_cmpgt_epi8( limit, index2 );
        const auto mask3 = _mm_cmpgt_epi8( limit, index3 );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( chunk ),
                          _mm_or_si128( _mm_and_si128( mask0, out0 ), _mm_andnot_si128( mask0, in0 ) ) );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( chunk + sizeof( __m128i ) ),
                          _mm_or_si128( _mm_and_si128( mask1, out1 ), _mm_andnot_si128( mask1, in1 ) ) );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( chunk + 2 * sizeof( __m128i ) ),
                          _mm_or_si128( _mm_and_si128( mask2, out2 ), _mm_andnot_si128( mask2, in2 ) ) );
        _mm_storeu_si128( reinterpret_cast<__m128i*>( chunk + 3 * sizeof( __m128i ) ),
                          _mm_or_si128( _mm_and_si128( mask3, out3 ), _mm_andnot_si128( mask3, in3 ) ) );
        return head;
    }
#endif
#if defined( __ARM_NEON )
    if ( position < 64 ) {
        auto* const data = reinterpret_cast<uint8_t*>( mtf.data() );
        const auto head = data[position];

        const auto in0 = vld1q_u8( data );
        const auto in1 = vld1q_u8( data + 16 );
        const auto in2 = vld1q_u8( data + 32 );
        const auto in3 = vld1q_u8( data + 48 );

        /* vext concatenates its operands, so [a, b] at offset 15 yields [a[15], b[0..14]]. */
        auto headVector = vdupq_n_u8( 0 );
        headVector = vsetq_lane_u8( head, headVector, 15 );
        const auto out0 = vextq_u8( headVector, in0, 15 );
        const auto out1 = vextq_u8( in0, in1, 15 );
        const auto out2 = vextq_u8( in1, in2, 15 );
        const auto out3 = vextq_u8( in2, in3, 15 );

        const auto limit = vdupq_n_u8( static_cast<uint8_t>( position + 1 ) );
        const auto index0 = vld1q_u8( MTF_INDEX_BYTES.data() );
        const auto index1 = vld1q_u8( MTF_INDEX_BYTES.data() + 16 );
        const auto index2 = vld1q_u8( MTF_INDEX_BYTES.data() + 32 );
        const auto index3 = vld1q_u8( MTF_INDEX_BYTES.data() + 48 );

        const auto mask0 = vcgtq_u8( limit, index0 );
        const auto mask1 = vcgtq_u8( limit, index1 );
        const auto mask2 = vcgtq_u8( limit, index2 );
        const auto mask3 = vcgtq_u8( limit, index3 );

        vst1q_u8( data, vbslq_u8( mask0, out0, in0 ) );
        vst1q_u8( data + 16, vbslq_u8( mask1, out1, in1 ) );
        vst1q_u8( data + 32, vbslq_u8( mask2, out2, in2 ) );
        vst1q_u8( data + 48, vbslq_u8( mask3, out3, in3 ) );
        return head;
    }
#endif

    const auto wordIndex = position >> 3;  // equivalent to division by sizeof( PackedMTF::value_type )
    const auto shift = static_cast<uint32_t>( ( position & 7U ) * 8U );
    uint64_t previous = ( mtf[wordIndex] >> shift ) & 0xFFU;

    for ( size_t i = 0; i < wordIndex; ++i ) {
        const auto current = mtf[i];
        mtf[i] = ( current << 8U ) | previous;
        previous = current >> 56U;
    }

    const auto current = mtf[wordIndex];
    /* 0x100 << 56 overflows to zero for unsigned, which would give the correct all-ones mask,
     * but spell it out so that the shift is never out of range. */
    const auto mask = shift == 56U ? ~uint64_t( 0 ) : ( ( uint64_t( 0x100 ) << shift ) - 1U );
    mtf[wordIndex] = ( current & ~mask ) | ( ( ( current << 8U ) | previous ) & mask );
    return static_cast<uint8_t>( mtf.front() );
}


#if defined( __ARM_NEON )
/**
 * @return four mask bits per byte, set for equal bytes, so that ctz(mask) / 4 is the first equal byte.
 */
[[nodiscard]] inline uint64_t
equalByteMask( const uint8x16_t equal ) noexcept
{
    const auto narrowed = vshrn_n_u16( vreinterpretq_u16_u8( equal ), 4 );
    return vget_lane_u64( vreinterpret_u64_u8( narrowed ), 0 );
}
#endif


/**
 * @return 1..9 representing the bzip2 block size of 100k to 900k
 */
inline uint8_t
readBzip2Header( BitReader& bitReader )
{
    for ( const auto magicByte : MAGIC_BYTES_BZ2 ) {
        const auto readByte = static_cast<char>( static_cast<uint8_t>( bitReader.read<8>() ) );
        if ( readByte != magicByte ) {
            std::stringstream msg;
            msg << "Input header is not BZip2 magic string 'BZh' (0x"
                << std::hex << int('B') << int('Z') << int('h') << std::dec << "). Mismatch at bit position "
                << bitReader.tell() - 8 << " with " << readByte << " (0x" << std::hex
                /* first cast to unsigned of same length so that the hex representation makes sense. */
                << static_cast<int>( static_cast<uint8_t>( readByte ) )
                << ") should be " << magicByte;
            throw std::domain_error( std::move( msg ).str() );
        }
    }

    // Next byte ASCII '1'-'9', indicates block size in units of 100k of
    // uncompressed data. Allocate intermediate buffer for block.
    const auto i = static_cast<char>( static_cast<uint8_t>( bitReader.read<8>() ) );
    if ( ( i < '1' ) || ( i > '9' ) ) {
        std::stringstream msg;
        msg << "Blocksize must be one of '0' (" << std::hex << static_cast<int>( '0' ) << ") ... '9' ("
            << static_cast<int>( '9' ) << ") but is " << i << " (" << static_cast<int>( i ) << ")";
        throw std::domain_error( std::move( msg ).str() );
    }

    return static_cast<uint8_t>( i - '0' );
}


struct Block  // NOLINT(clang-analyzer-optin.performance.Padding)
{
public:
    /**
     * m ibzip2 && src/tools/ibzip2 -P 1 -f -L offsets -i scorep-pragzip-bgzf-16MiB.tar.bz2
     * @verbatim
     * [BZ2Reader] Time spent:
     * decodeBlock                   : 2.99293s
     * readBlockHeader               : 17.9826s
     *     readSymbolMaps            : 0.000444198s
     *     readSelectors             : 0.0294166s
     *     readTrees                 : 0.0168973s
     *     createHuffmanTable        : 16.8445s
     *     burrowsWheelerPreparation : 1.08934s
     * @endverbatim
     */
    struct Statistics
    {
    public:
        void
        merge( const Statistics& other )
        {
            durations.merge( other.durations );
        }

    public:
        struct Durations
        {
        public:
            void
            merge( const Durations& other )
            {
                readBlockHeader           += other.readBlockHeader;
                decodeBlock               += other.decodeBlock;
                readSymbolMaps            += other.readSymbolMaps;
                readSelectors             += other.readSelectors;
                readTrees                 += other.readTrees;
                createHuffmanTable        += other.createHuffmanTable;
                burrowsWheelerPreparation += other.burrowsWheelerPreparation;
            }

        public:
            double readBlockHeader{ 0 };
            double decodeBlock{ 0 };

            /* Parts of readBlockHeader. */
            double readSymbolMaps{ 0 };
            double readSelectors{ 0 };
            double readTrees{ 0 };
            double createHuffmanTable{ 0 };
            double burrowsWheelerPreparation{ 0 };
        };

        Durations durations;
    };

    //using HuffmanCoding = rapidgzip::HuffmanCodingLinearSearch<uint32_t, uint16_t>;
    //using HuffmanCoding = rapidgzip::HuffmanCodingSymbolsPerLength<uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS,
    //                                                               /* CHECK_OPTIMALITY */ false>;
    /**
     * Some quick benchmarks with m ibzip2 && time src/tools/ibzip2 -v -P 1 -d -o /dev/null -f silesia.tar.bz2:
     *  4-bit: 4.421 4.421 4.482 4.591 4.634
     *  6-bit: 4.462 4.506 4.277 4.279 4.485
     *  8-bit: 4.198 4.188 4.176 4.188 4.297
     * 10-bit: 4.203 4.046 4.174 4.164 4.198
     * 11-bit: 4.117 4.249 4.137 4.076 4.073
     * 12-bit: 3.970 4.101 4.177 4.047 4.030 <-
     * 13-bit: 4.199 4.188 4.144 4.102 4.162
     * 14-bit: 4.134 4.217 4.240 4.108 4.244
     * 16-bit: 4.438 4.415 4.306 4.476 4.577
     * 18-bit: 4.542 4.624 4.562 4.581 4.862
     * Tested on AMD Ryzen 3900X.
     */
    using HuffmanCoding = rapidgzip::HuffmanCodingShortBitsCached<
        uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS, /* LUT size */ 12,
        /* REVERSE_BITS */ false, /* CHECK_OPTIMALITY */ false
    >;
    /* Won't work. It will cause a stack overflow because it contains a std::array of size 2^MAX_HUFCODE_BITS = 1 MiB.
     * And the currently committed version also will not work because it reverses the bits as necessary for gzip.
     * I think this step must be skipped for the bzip2 BitReader. */
    //using HuffmanCoding =
    //    rapidgzip::HuffmanCodingReversedBitsCached<uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS>;

public:
    Block() = default;

    ~Block() = default;

    /** Better don't allow copies because the bitreader would be shared, which might be problematic */
    Block( const Block& ) = delete;

    Block&
    operator=( const Block& ) = delete;

    Block( Block&& ) = delete;

    Block&
    operator=( Block&& ) = delete;

    explicit
    Block( BitReader& bitReader )
    {
        reset( bitReader );
    }

    /**
     * Reinitialize this block for a new block header while keeping the allocated BWT buffers. This avoids
     * allocating and freeing the multi-megabyte buffers for every bzip2 block.
     */
    void
    reset( BitReader& bitReader )
    {
        m_bitReader = &bitReader;
        readBlockHeader();
    }

    /**
     * First pass, read block's symbols into bwtData[bwtDataCount].
     *
     * This undoes three types of compression: huffman coding, run length encoding,
     * and move to front encoding.  We have to undo all those to know when we've
     * read enough input.
     *
     * It is not automatically called by the constructor and must be called manually for non-EOS blocks.
     * The interface is like this because ParallelBZ2Reader slows down when calling it automatically
     * because it would not be called on the worker threads but on the main thread!
     */
    void
    readBlockData();

    /**
     * @return True if this is a special enf-of-stream bzip2 block, which contains no data.
     */
    [[nodiscard]] constexpr bool
    eos() const noexcept
    {
        return m_atEndOfStream;
    }

    /**
     * @return True if all data has been read from this block.
     */
    [[nodiscard]] constexpr bool
    eob() const noexcept
    {
        return eos() || !bwdata.hasData();
    }

    [[nodiscard]] constexpr bool
    eof() const noexcept
    {
        return m_atEndOfFile;
    }

    [[nodiscard]] BitReader&
    bitReader()
    {
        if ( m_bitReader != nullptr ) {
            return *m_bitReader;
        }
        throw std::invalid_argument( "Block has not been initialized yet!" );
    }

    /**
     * Currently, the logic is limited and might write up to nMaxBytesToDecode + 255 characters
     * to the output buffer! Currently, the caller has to ensure that the output buffer is large enough.
     */
    [[nodiscard]] size_t
    read( const size_t nMaxBytesToDecode,
          char*        outputBuffer )
    {
        const auto t0 = rapidgzip::now();
        const auto result = bwdata.decodeBlock( nMaxBytesToDecode, outputBuffer );
        statistics.durations.decodeBlock += rapidgzip::duration( t0 );
        return result;
    }

    /**
     * @return The current CRC32 checksum of the decoded data. If all the data of this block has been decoded,
     *         this should match header CRC.
     */
    [[nodiscard]] constexpr uint32_t
    dataCRC() const noexcept
    {
        return bwdata.dataCRC;
    }

    /**
     * @return The CRC32 checksum as stored in the bzip2 block header.
     */
    [[nodiscard]] constexpr uint32_t
    headerCRC() const noexcept
    {
        return bwdata.headerCRC;
    }

    /**
     * Scan the RLE1-encoded BWT output once to determine the exact decoded size of this block.
     * This is only valid after readBlockData(), which produces the RLE1-encoded output.
     */
    [[nodiscard]] size_t
    decodedSize() const noexcept
    {
        return bwdata.decodedSize();
    }

private:
    template<uint8_t nBits>
    [[nodiscard]] uint32_t
    getBits()
    {
        return static_cast<uint32_t>( bitReader().read<nBits>() );
    }

    [[nodiscard]] uint32_t
    getBits( uint8_t nBits )
    {
        return static_cast<uint32_t>( bitReader().read( nBits ) );
    }

    void
    readBlockHeader();

    void
    readBlockTrees()
    {
        const auto tReadSymbolMaps = rapidgzip::now();
        readSymbolMaps();
        const auto tReadSelectors = rapidgzip::now();
        readSelectors();
        const auto tReadTrees = rapidgzip::now();
        readTrees();

        statistics.durations.readSymbolMaps += rapidgzip::duration( tReadSymbolMaps, tReadSelectors );
        statistics.durations.readSelectors += rapidgzip::duration( tReadSelectors, tReadTrees );
        statistics.durations.readTrees += rapidgzip::duration( tReadTrees );
    }

    void
    readSymbolMaps();

    void
    readSelectors();

    void
    readTrees();

public:
    struct BurrowsWheelerTransformData
    {
        BurrowsWheelerTransformData() = default;
        BurrowsWheelerTransformData( const BurrowsWheelerTransformData& ) = delete;
        BurrowsWheelerTransformData& operator=( const BurrowsWheelerTransformData& ) = delete;

        ~BurrowsWheelerTransformData()
        {
            if ( unbwtContext != nullptr ) {
                libsais_unbwt_free_ctx( unbwtContext );
            }
        }

        friend Block;

    public:
        /**
         * Currently, the logic is limited and might write up to nMaxBytesToDecode + 255 characters
         * to the output buffer! Currently, the caller has to ensure that the output buffer is large enough.
         */
        [[nodiscard]] size_t
        decodeBlock( size_t nMaxBytesToDecode,
                     char*  outputBuffer );

        [[nodiscard]] constexpr bool
        hasData() const noexcept
        {
            if ( inverseBWT ) {
                return ( inverseBWT->position < writeCount ) || ( inverseBWT->repeatCount > 0 );
            }
            return ( writeCount > 0 ) || ( symbolRepeatCount > 0 );
        }

        /**
         * @return The number of bytes that decodeBlock will produce, i.e., the result of expanding the
         *         RLE1 runs in bwtData. A run consists of four identical bytes followed by a count byte
         *         and decodes to 4 + count bytes. Runs shorter than five bytes at the end are literals.
         */
        [[nodiscard]] size_t
        decodedSize() const noexcept
        {
            size_t decodedSize = 0;
            uint32_t position = 0;

        #if LIBRAPIDARCHIVE_BZIP2_HAS_SSE2
            /* Test four shifted 16-byte windows at once to find the next run start. Positions before it
             * are literals, which avoids one branch and comparison per byte for literal-heavy blocks. */
            while ( position + 20 <= writeCount ) {
                const auto* const source = bwtData.get() + position;
                const auto v0 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source ) );
                const auto v1 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source + 1 ) );
                const auto v2 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source + 2 ) );
                const auto v3 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source + 3 ) );
                const auto equal = _mm_and_si128( _mm_and_si128( _mm_cmpeq_epi8( v0, v1 ),
                                                                 _mm_cmpeq_epi8( v0, v2 ) ),
                                                  _mm_cmpeq_epi8( v0, v3 ) );
                const auto runStartMask = static_cast<uint32_t>( _mm_movemask_epi8( equal ) );
                if ( runStartMask == 0 ) {
                    decodedSize += 16;
                    position += 16;
                    continue;
                }

                const auto literalCount = static_cast<uint32_t>( rapidgzip::trailingZeroCount( runStartMask ) );
                decodedSize += literalCount;
                position += literalCount;
                decodedSize += 4 + bwtData[position + 4];
                position += 5;
            }
        #endif

            for ( ; position < writeCount; ) {
                const auto byte = bwtData[position];
                if ( ( position + 4 < writeCount )
                     && ( bwtData[position + 1] == byte )
                     && ( bwtData[position + 2] == byte )
                     && ( bwtData[position + 3] == byte ) ) {
                    decodedSize += 4 + bwtData[position + 4];
                    position += 5;
                } else {
                    ++decodedSize;
                    ++position;
                }
            }
            return decodedSize;
        }

    private:
        /** Must only be called after initializing the members from outside. */
        void
        prepare();

        /**
         * Reset the per-block state while retaining the allocated dbuf and bwtData buffers for reuse.
         * The header and symbol pass of the next block overwrite everything that is read.
         */
        void
        resetState()
        {
            origPtr = 0;
            byteCount.fill( 0 );
            writePos = 0;
            writeRun = 0;
            writeCount = 0;
            writeCurrent = 0;
            symbolToRepeat = 0;
            symbolRepeatCount = 0;
            inverseBWT.reset();
            dataCRC = 0xFFFFFFFFL;
            headerCRC = 0;
        }

    private:
        uint32_t origPtr = 0;
        std::array<uint32_t, 256> byteCount{};

        /* These variables are saved when interrupting decode and are required for resuming */
        uint32_t writePos = 0;
        int writeRun = 0;
        uint32_t writeCount = 0;
        int writeCurrent = 0;

        /* This is for resuming run-length compression. */
        uint8_t symbolToRepeat{ 0 };
        uint8_t symbolRepeatCount{ 0 };

        /* libsais inverts the whole block linearly. Its output is still run-length encoded, so one run may
         * span several decodeBlock calls. The presence of this state selects the libsais path. */
        struct InverseBWTData
        {
            size_t position{ 0 };
            size_t repeatCount{ 0 };
            uint8_t repeatByte{ 0 };
        };

        std::optional<InverseBWTData> inverseBWT;

        /* libsais allocates ~1 MB of inverse-BWT tables per call. Keep one context per Block/worker so that
         * the tables and their pages are reused across blocks instead of being allocated and trimmed. */
        void* unbwtContext{ nullptr };

        uint32_t dataCRC = 0xFFFFFFFFL;  /* CRC of block as calculated by us */
        uint32_t headerCRC = 0;  /* what the block data CRC should be */

        /**
         * Simply allocate the maximum block size so we won't run into problem when block sizes changes
         * (e.g. in pbzip2 file). MAX_BLOCK_SIZE is nothing in today's age anyways.
         * The extra element is required because libsais_unbwt_cyclic uses this buffer as its n + 1 element
         * temporary P array when the inverse BWT is computed.
         */
        /* Not zero-initialized: the symbol pass and libsais overwrite every element that is read. The buffers
         * are allocated once per reader or worker, so this saves a one-time 4.5 MiB of zeroing at startup. */
        std::unique_ptr<uint32_t[]> dbuf{ new uint32_t[MAX_BLOCK_SIZE + 1] };

        /* BWT input bytes. libsais inverts them in place into the run-length encoded output; the classic
         * chase fallback copies them into dbuf first. Not zero-initialized because the symbol pass and
         * libsais overwrite every element that is read. The eight tail bytes are slack for the word-store
         * run flush, which may write past the logical count. */
        std::unique_ptr<uint8_t[]> bwtData{ new uint8_t[MAX_BLOCK_SIZE + sizeof( uint64_t )] };
    };

public:
    Statistics statistics;

    size_t encodedOffsetInBits = 0;
    size_t encodedSizeInBits = 0;

private:
    uint64_t magicBytes{ 0 };
    bool isRandomized{ false };

    /* First pass decompression data (Huffman and MTF decoding) */

    /**
     * mapping table: if some byte values are never used (encoding things
     * like ascii text), the compression code removes the gaps to have fewer
     * symbols to deal with, and writes a sparse bitfield indicating which
     * values were present.  We make a translation table to convert the symbols
     * back to the corresponding bytes.
     */
    std::array<uint8_t, 256> symbolToByte{};
    /** Used for the selectors, which are decoded before the block data. */
    std::array<uint8_t, 256> mtfSymbol{};
    /** Used for the block data, which has a much larger and less skewed index distribution. */
    PackedMTF mtfPacked{};
    unsigned int symbolCount{ 0 };
    /**
     * Every GROUP_SIZE many symbols we switch huffman coding tables.
     * Each group has a selector, which is an index into the huffman coding table arrays.
     *
     * Read in the group selector array, which is stored as MTF encoded
     * bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
     * to the front of the table, so it has a shorter encoding next time.)
     */
    uint16_t selectorsCount{ 0 };

    std::array<char, 32768> selectors{};  // nSelectors=15 bits
    std::array<HuffmanCoding, MAX_GROUPS> huffmanCodings{};
    uint32_t groupCount = 0;

    /* Second pass decompression data (burrows-wheeler transform) */
    BurrowsWheelerTransformData bwdata;

    BitReader* m_bitReader = nullptr;
    bool m_atEndOfStream = false;
    bool m_atEndOfFile = false;
};


/* Read block header at start of a new compressed data block.  Consists of:
 *
 * 48 bits : Block signature, either pi (data block) or e (EOF block).
 * 32 bits : bw->headerCRC
 * 1  bit  : obsolete feature flag.
 * 24 bits : origPtr (Burrows-wheeler unwind index, only 20 bits ever used)
 * 16 bits : Mapping table index.
 *[16 bits]: symToByte[symTotal] (Mapping table.  For each bit set in mapping
 *           table index above, read another 16 bits of mapping table data.
 *           If corresponding bit is unset, all bits in that mapping table
 *           section are 0.)
 *  3 bits : groupCount (how many huffman tables used to encode, anywhere
 *           from 2 to MAX_GROUPS)
 * variable: hufGroup[groupCount] (MTF encoded huffman table data.)
 */
inline void
Block::readBlockHeader()
{
    const auto tReadBlockHeader = rapidgzip::now();

    /* A Block may be reused for many headers. Reset the per-block state and timing while keeping the
     * expensive buffers allocated. */
    statistics = {};
    bwdata.resetState();
    m_atEndOfFile = false;

    encodedOffsetInBits = bitReader().tell();
    encodedSizeInBits = 0;

    magicBytes = ( (uint64_t)getBits<24>() << 24U ) | (uint64_t)getBits<24>();
    bwdata.headerCRC = getBits( 32 );
    m_atEndOfStream = magicBytes == MAGIC_BITS_EOS;
    if ( m_atEndOfStream ) {
        /* read byte padding bits */
        const auto nBitsInByte = static_cast<uint8_t>( bitReader().tell() & 7LLU );
        if ( nBitsInByte > 0 ) {
            bitReader().read( uint8_t( 8 ) - nBitsInByte );
        }

        encodedSizeInBits = bitReader().tell() - encodedOffsetInBits;
        m_atEndOfFile = bitReader().eof();
        return;
    }

    if ( magicBytes != MAGIC_BITS_BLOCK ) {
        std::stringstream msg;
        msg << "[BZip2 block header] invalid compressed magic 0x" << std::hex << magicBytes
            << " at offset " << rapidgzip::formatBits( encodedOffsetInBits );
        throw std::domain_error( std::move( msg ).str() );
    }

    isRandomized = getBits<1>() != 0;
    if ( isRandomized ) {
        throw std::domain_error( "[BZip2 block header] deprecated isRandomized bit is not supported" );
    }

    bwdata.origPtr = getBits<24>();
    if ( bwdata.origPtr > MAX_BLOCK_SIZE ) {
        std::stringstream msg;
        msg << "[BZip2 block header] origPtr " << bwdata.origPtr << " is larger than the maximum block size: "
            << MAX_BLOCK_SIZE;
        throw std::logic_error( std::move( msg ).str() );
    }

    readBlockTrees();
    statistics.durations.readBlockHeader += rapidgzip::duration( tReadBlockHeader );
}


inline void
Block::readSymbolMaps()
{
    /**
     * The Mapping table itself is compressed in two parts:
     * huffmanUsedMap: each bit indicates whether the corresponding range [0...15], [16...31] is present.
     * huffman_used_bitmaps: 0-16 16-bit bitmaps
     * The Huffman map gives 0, 10, 11, 100, 101, ... (8-bit) symbols
     * Instead of storing 2 * 256 bytes ( 0b : A, 10b : B, .. ) for the table, the first part is left out.
     * And for short maps, only the first n-th are actually stored.
     * The second half is also assumed to be ordered, so that we only need to store which symbols are actually
     * present.
     * This however means that the Huffmann table can't be frequency sorted, therefore this is done in a
     * second step / table, the mtfSymbol (move to front) map.
     * This would need 256 bits to store the table in huffman_used_bitmaps.
     * These bits are split in groups of 16 and the presence of each group is encoded in huffmanUsedMap
     * to save even more bytes.
     * @verbatim
     *  10001000 00000000     # huffmanUsedMap (bit map)
     *  ^   ^
     *  |   [64,95]
     *  [0...15]
     *  00000000 00100000     # huffman_used_bitmaps[0]
     *  ^          ^    ^
     *  0          10   15
     *          (newline)
     *  00000100 10001001     # huffman_used_bitmaps[1]
     *  ^    ^   ^   ^  ^
     *  64   69  72  76 95
     *       E   H   L  O
     * @endverbatim
     */
    const uint16_t huffmanUsedMap = getBits<16>();
    /* Can at most grow up to 256 symbols, i.e., MAX_SYMBOLS - 2 (RUNA, RUNB). */
    symbolCount = 0;
    for ( int i = 0; i < 16; i++ ) {
        if ( ( huffmanUsedMap & ( 1U << ( 15U - i ) ) ) != 0 ) {
            const auto bitmap = getBits<16>();
            for ( int j = 0; j < 16; j++ ) {
                if ( ( bitmap & ( 1U << ( 15U - j ) ) ) != 0 ) {
                    symbolToByte[symbolCount++] = ( 16 * i ) + j;
                }
            }
        }
    }
}


inline void
Block::readSelectors()
{
    // How many different huffman coding groups does this block use?
    groupCount = getBits<3>();
    if ( ( groupCount < 2 ) || ( groupCount > MAX_GROUPS ) ) {
        std::stringstream msg;
        msg << "[BZip2 block header] Invalid Huffman coding group count " << groupCount;
        throw std::logic_error( std::move( msg ).str() );
    }

    // nSelectors: Every GROUP_SIZE many symbols we switch huffman coding
    // tables.  Each group has a selector, which is an index into the huffman
    // coding table arrays.
    //
    // Read in the group selector array, which is stored as MTF encoded
    // bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
    // to the front of the table, so it has a shorter encoding next time.)
    selectorsCount = getBits<15>();
    if ( selectorsCount == 0 ) {
        std::stringstream msg;
        msg << "[BZip2 block header] The number of selectors " << selectorsCount << " is invalid";
        throw std::logic_error( std::move( msg ).str() );
    }

    /* The "selectors", referring to the Huffman trees to decode a group of 50 symbols, can only range from 0-5,
     * because MAX_GROUPS = 6, and are encoded as zero-terminated 1-bits, where the number of 1-bits represents
     * the Huffman tree ID. */
    static constexpr std::array<uint8_t, ( 1U << MAX_GROUPS )> BITS_TO_SELECTOR =
        [] () {
            std::array<uint8_t, ( 1U << MAX_GROUPS )> result{};
            uint8_t lowestBitsSet{ 0 };
            for ( uint8_t selector = 0; selector < MAX_GROUPS; ++selector ) {
                const auto paddingBitsCount = MAX_GROUPS - selector;
                const auto maxPaddingBits = static_cast<uint8_t>( 1U << paddingBitsCount );
                const auto highestBitsSet = static_cast<uint8_t>( lowestBitsSet << ( MAX_GROUPS - selector ) );
                for ( uint8_t paddingBits = 0; paddingBits < maxPaddingBits; ++paddingBits ) {
                    result[highestBitsSet | paddingBits] = selector;
                }

                lowestBitsSet <<= 1U;
                lowestBitsSet |= 1U;
            }
        /* A full line of 1-bits is not allowed because it would indicate a selector index larger than MAX_GROUPS. */
            result.back() = MAX_GROUPS;
            return result;
        } ();

    std::iota( mtfSymbol.begin(), mtfSymbol.begin() + groupCount, 0 );
    for ( size_t i = 0; i < selectorsCount; i++ ) {
        const auto j = BITS_TO_SELECTOR.at( m_bitReader->peek<MAX_GROUPS>() );
        if ( j >= groupCount ) {
            std::stringstream msg;
            msg << "[BZip2 block header] Could not find zero termination after " << groupCount << " bits";
            throw std::domain_error( std::move( msg ).str() );
        }
        /* Validate before consuming because a full line of MAX_GROUPS 1-bits has no zero terminator to seek past. */
        m_bitReader->seekAfterPeek( j + 1 );

        // Decode MTF to get the next selector, and move it to the front.
        const auto uc = mtfSymbol[j];
        memmove( mtfSymbol.data() + 1, mtfSymbol.data(), j );
        mtfSymbol[0] = uc;
        selectors[i] = static_cast<char>( uc );
    }
}


/**
 * bzip2 blocks are many times larger than usual gzip blocks. That's why multiple Huffman tree per block are
 * supported and necessary. Similarly to deflate, the trees are stores as code lengths per symbol.
 */
inline void
Block::readTrees()
{
    // Read the huffman coding tables for each group, which code for symTotal
    // literal symbols, plus two run symbols (RUNA, RUNB)
    const auto symCount = symbolCount + 2;
    for ( size_t j = 0; j < groupCount; j++ ) {
        // Read lengths
        std::array<uint8_t, MAX_SYMBOLS> lengths{};
        unsigned int hh = getBits<5>();
        for ( unsigned int symbol = 0; symbol < symCount; symbol++ ) {
            while ( true ) {
                // !hh || hh > MAX_HUFCODE_BITS in one test.
                if ( MAX_HUFCODE_BITS - 1 < hh - 1 ) {
                    std::stringstream msg;
                    msg << "[BZip2 block header] start_huffman_length " << hh
                        << " is larger than " << MAX_HUFCODE_BITS << " or zero\n";
                    throw std::logic_error( std::move( msg ).str() );
                }

                // Stop if first bit is 0, otherwise second bit says whether to
                // increment or decrement.
                if ( getBits<1>() != 0 ) {
                    hh += 1 - ( getBits<1>() << 1U );
                } else {
                    break;
                }
            }
            if ( hh > std::numeric_limits<uint8_t>::max() ) {
                std::stringstream msg;
                msg << "[BZip2 block header] The read code length is unexpectedly large: " << hh;
                throw std::logic_error( std::move( msg ).str() );
            }
            lengths[symbol] = static_cast<uint8_t>( hh );
        }

        const auto error = huffmanCodings[j].initializeFromLengths(
            rapidgzip::VectorView<uint8_t>( lengths.data(), symCount ) );
        if ( error != rapidgzip::Error::NONE ) {
            throw std::domain_error( toString( error ) );
        }
    }
}


/**
 * This undoes the Huffman coding.
 */
inline void
Block::readBlockData()
{
    // We've finished reading and digesting the block.  Now read this
    // block's huffman coded symbols from the file and undo the huffman coding
    // and run length encoding, saving the result into bwtData[bwtDataCount++] = uc

    bwdata.byteCount.fill( 0 );
    static constexpr auto PACKED_MTF_INITIAL = createPackedMTF();
    mtfPacked = PACKED_MTF_INITIAL;

    const auto t0 = rapidgzip::now();
    /** @note The loops inside this for-loop are all too short to be profiled.
     *        The overhead becomes disastrously large! It takes 190s to decode instead of 20s. */
    // Loop through compressed symbols.  This is the first "tight inner loop"
    // that needs to be micro-optimized for speed.  (This one fills out bwtData[]
    // linearly, staying in cache more, so isn't as limited by DRAM access.)
    uint32_t bwtDataCount = 0;
    auto& reader = *m_bitReader;

    /* The weight of the next RUNA/RUNB symbol. It starts at one after every literal, so each run
     * symbol can emit its share immediately instead of accumulating a pending run that a later literal
     * has to flush. Short runs use one 64-bit store and leave the slack bytes for the next write. */
    uint32_t runWeight = 1;
    const auto appendRunBytes =
        [&] ( const uint8_t uc, const uint32_t count )
        {
            bwdata.byteCount[uc] += count;
            if ( count <= sizeof( uint64_t ) ) {
                const auto pattern = uint64_t( 0x0101'0101'0101'0101ULL ) * uc;
                std::memcpy( bwdata.bwtData.get() + bwtDataCount, &pattern, sizeof( pattern ) );
                bwtDataCount += count;
            } else {
                for ( uint32_t i = 0; i < count; ++i ) {
                    bwdata.bwtData[bwtDataCount++] = uc;
                }
            }
        };

    // Determine which huffman coding group to use.
    uint32_t selector = 0;
    if ( selector >= selectorsCount ) {
        std::stringstream msg;
        msg << "[BZip2 block data] selector " << selector << " out of maximum range " << selectorsCount;
        throw std::domain_error( std::move( msg ).str() );
    }
    const auto* huffmanCoding = &huffmanCodings[selectors[selector]];
    selector++;

    auto nextSym = huffmanCoding->decodePacked( reader );
    if ( UNLIKELY( nextSym == HuffmanCoding::INVALID_SYMBOL ) ) [[unlikely]] {
        throw std::domain_error( "[BZip2 block data] Invalid Huffman code" );
    }

    uint32_t symbolsInGroup = GROUP_SIZE;
    while ( true ) {
        // Is this the terminating symbol?
        if ( nextSym > symbolCount ) {
            break;
        }

        /* Select the group of the successor before decoding it, then decode the successor before consuming
         * the current symbol. The successor's bit-reader and Huffman LUT loads then overlap the 64-byte
         * move-to-front store of the current literal instead of waiting behind it. */
        if ( --symbolsInGroup == 0 ) {
            if ( selector >= selectorsCount ) {
                std::stringstream msg;
                msg << "[BZip2 block data] selector " << selector << " out of maximum range " << selectorsCount;
                throw std::domain_error( std::move( msg ).str() );
            }
            huffmanCoding = &huffmanCodings[selectors[selector]];
            selector++;
            symbolsInGroup = GROUP_SIZE;
        }

        const auto currentSym = nextSym;
        nextSym = huffmanCoding->decodePacked( reader );
        if ( UNLIKELY( nextSym == HuffmanCoding::INVALID_SYMBOL ) ) [[unlikely]] {
            throw std::domain_error( "[BZip2 block data] Invalid Huffman code" );
        }

        // If this is a repeated run, emit this symbol's share immediately.
        if ( currentSym <= SYMBOL_RUNB ) {
            /* The run length must fit into one block. Check in 64 bit because a crafted sequence of
             * RUNA/RUNB symbols can wrap the 32-bit sum and bypass the block-size check, which would
             * then write far past the block buffer. */
            const auto count = static_cast<uint64_t>( runWeight ) << currentSym;
            if ( static_cast<uint64_t>( bwtDataCount ) + count > MAX_BLOCK_SIZE ) {
                std::stringstream msg;
                msg << "[BZip2 block data] The run length exceeds the maximum block size " << MAX_BLOCK_SIZE;
                throw std::domain_error( std::move( msg ).str() );
            }
            appendRunBytes( symbolToByte[mtfPacked[0] & 0xFFU], static_cast<uint32_t>( count ) );
            runWeight <<= 1U;
            continue;
        }

        /* At this point, the symbol we just decoded indicates a new literal
         * character. Subtract one to get the position in the MTF array
         * at which this literal is currently to be found. (Note that the
         * result can't be -1 or 0, because 0 and 1 are RUNA and RUNB.
         * Another instance of the first symbol in the mtf array, position 0,
         * would have been handled as part of a run.) */
        if ( bwtDataCount >= MAX_BLOCK_SIZE ) {
            std::stringstream msg;
            msg << "[BZip2 block data] bwtDataCount " << bwtDataCount
                << " >= " << MAX_BLOCK_SIZE << " maximum block size";
            throw std::domain_error( std::move( msg ).str() );
        }
        const auto uc = symbolToByte[moveToFront( mtfPacked, currentSym - 1 )];

        // We have our literal byte.  Save it into bwtData.
        bwdata.byteCount[uc]++;
        bwdata.bwtData[bwtDataCount++] = uc;
        runWeight = 1;
    }

    // Now we know what bwtDataCount is, do a better sanity check on origPtr.
    bwdata.writeCount = bwtDataCount;
    if ( bwdata.origPtr >= bwtDataCount ) {
        std::stringstream msg;
        msg << "[BZip2 block data] origPtr error " << bwdata.origPtr;
        throw std::domain_error( std::move( msg ).str() );
    }

    statistics.durations.createHuffmanTable += rapidgzip::duration( t0 );

    const auto tPrepareStart = rapidgzip::now();
    bwdata.prepare();
    statistics.durations.burrowsWheelerPreparation += rapidgzip::duration( tPrepareStart );

    encodedSizeInBits = bitReader().tell() - encodedOffsetInBits;
}


inline void
Block::BurrowsWheelerTransformData::prepare()
{
    dataCRC = 0xFFFFFFFFL;
    symbolRepeatCount = 0;
    inverseBWT.reset();

    if ( writeCount == 0 ) {
        return;
    }

    /* libsais builds a bi-gram LF table and decodes several chains in parallel, which hides most of the
     * dependent-load latency of the classic one-step chase. The frequency table is already available from
     * the symbol decoding pass. */
    std::array<int32_t, 256> frequencies{};
    for ( size_t i = 0; i < byteCount.size(); ++i ) {
        frequencies[i] = static_cast<int32_t>( byteCount[i] );
    }

    /* libsais requires n + 1 int32 elements of temporary space, which dbuf provides. Inverting in place
     * removes the separate output buffer. On error, bwtData is still the unmodified input because libsais
     * validates and allocates all of its tables before writing, so the chase fallback below stays valid. */
    /* dbuf has MAX_BLOCK_SIZE + 1 elements; libsais needs n + 1. The symbol pass bounds writeCount, but
     * keep the capacity assertion. */
    assert( writeCount <= MAX_BLOCK_SIZE );
    if ( unbwtContext == nullptr ) {
        unbwtContext = libsais_unbwt_create_ctx();
    }
    const auto error = unbwtContext != nullptr
                       ? libsais_unbwt_cyclic_ctx(
        unbwtContext, bwtData.get(), bwtData.get(), reinterpret_cast<int32_t*>( dbuf.get() ),
        static_cast<int32_t>( writeCount ), frequencies.data(), static_cast<int32_t>( origPtr ) )
                       : libsais_unbwt_cyclic(
        bwtData.get(), bwtData.get(), reinterpret_cast<int32_t*>( dbuf.get() ),
        static_cast<int32_t>( writeCount ), frequencies.data(), static_cast<int32_t>( origPtr ) );
    if ( error == 0 ) {
        inverseBWT = InverseBWTData{};
        /* Keep dbuf allocated. The next block reuses it as libsais temporary space, which avoids a
         * multi-megabyte allocation and page-fault storm for every block. */
        return;
    }

    /* Fall back to the classic one-step LF build if libsais cannot allocate its temporary tables. */
    for ( uint32_t i = 0; i < writeCount; ++i ) {
        dbuf[i] = bwtData[i];
    }

    /**
     * Turn byteCount into cumulative occurrence counts of 0 to n-1.
     * @note This loop is fast because byteCount.size() is 256.
     */
    for ( size_t i = 0, cumulativeCount = 0; i < byteCount.size(); ++i ) {
        const auto newCumulativeCount = cumulativeCount + byteCount[i];
        byteCount[i] = static_cast<uint32_t>( cumulativeCount );
        cumulativeCount = newCumulativeCount;
    }

    // Use occurrence counts to quickly figure out what order dbuf would be in
    // if we sorted it.
    // Using i as position, j as previous character, hh as current character,
    // and uc as run count.
    for ( uint32_t i = 0; i < writeCount; i++ ) {
        const auto uc = static_cast<uint8_t>( dbuf[i] );
        dbuf[byteCount[uc]] |= i << 8U;
        byteCount[uc]++;
    }

    /* Decode first byte by hand to initialize "previous" byte. Note that it
     * doesn't get output, and if the first three characters are identical
     * it doesn't qualify as a run (hence uc=255, which will either wrap
     * to 1 or get reset). */
    writePos = dbuf[origPtr];
    writeCurrent = static_cast<uint8_t>( writePos & 0xFFU );
    writePos >>= 8U;
    writeRun = -1;
}


inline size_t
Block::BurrowsWheelerTransformData::decodeBlock( const size_t nMaxBytesToDecode,
                                                 char*        outputBuffer )
{
    if ( ( outputBuffer == nullptr ) || !hasData() ) {
        return 0;
    }

    size_t nBytesDecoded = 0;
    uint32_t crc = dataCRC;

    if ( inverseBWT ) {
        auto& state = *inverseBWT;
        while ( nBytesDecoded < nMaxBytesToDecode ) {
            if ( state.repeatCount > 0 ) {
                const auto nBytesToRepeat = std::min( state.repeatCount, nMaxBytesToDecode - nBytesDecoded );
                for ( size_t i = 0; i < nBytesToRepeat; ++i ) {
                    outputBuffer[nBytesDecoded++] = static_cast<char>( state.repeatByte );
                }
                state.repeatCount -= nBytesToRepeat;
                continue;
            }

            if ( state.position >= writeCount ) {
                break;
            }

        #if LIBRAPIDARCHIVE_BZIP2_AVX512_SCAN
            if ( hasAvx512RLEScan() ) {
                expandRLEAvx512( bwtData.get(), writeCount, state.position, outputBuffer,
                                 nBytesDecoded, nMaxBytesToDecode, state.repeatByte, state.repeatCount );
                if ( ( nBytesDecoded >= nMaxBytesToDecode ) || ( state.position >= writeCount ) ) {
                    break;
                }
            }
        #endif

        #if LIBRAPIDARCHIVE_BZIP2_HAS_SSE2 || defined( __ARM_NEON )
            /* Scan 16 positions at once. The scalar loop short-circuits after the first mismatch, but it still costs
             * one load and one compare per literal. A 128-bit/16-byte compare finds the first run start and lets whole
             * blocks of literals be copied with one load and one store. */
            bool handledRun = false;
            while ( ( nBytesDecoded + 16 <= nMaxBytesToDecode )
                    && ( state.position + 20 <= writeCount ) ) {
                const auto* const source = bwtData.get() + state.position;
                /* Simply load from byte-shifted offsets instead of load + shift to avoid instruction dependencies
                 * and to effectively load 16 + 3 bytes as we need to compare 4 subsequent bytes for each position. */
            #if LIBRAPIDARCHIVE_BZIP2_HAS_SSE2
                const auto v0 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source ) );
                const auto v1 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source + 1 ) );
                const auto v2 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source + 2 ) );
                const auto v3 = _mm_loadu_si128( reinterpret_cast<const __m128i*>( source + 3 ) );
                /* all four neighbors equal = ( ( v0 == v1 ) & ( v0 == v2 ) ) & ( v0 == v3 ) */
                const auto equal = _mm_and_si128( _mm_and_si128( _mm_cmpeq_epi8( v0, v1 ),
                                                                 _mm_cmpeq_epi8( v0, v2 ) ),
                                                  _mm_cmpeq_epi8( v0, v3 ) );
                /* Fast-path for only literals, no run-lengths. */
                const auto runStartMask = static_cast<uint32_t>( _mm_movemask_epi8( equal ) );
                if ( runStartMask == 0 ) {
                    _mm_storeu_si128( reinterpret_cast<__m128i*>( outputBuffer + nBytesDecoded ), v0 );
                    nBytesDecoded += sizeof( __m128i );
                    state.position += sizeof( __m128i );
                    continue;
                }

                const auto literalCount = static_cast<size_t>( rapidgzip::trailingZeroCount( runStartMask ) );
            #else
                const auto v0 = vld1q_u8( source );
                const auto v1 = vld1q_u8( source + 1 );
                const auto v2 = vld1q_u8( source + 2 );
                const auto v3 = vld1q_u8( source + 3 );
                const auto equal = vandq_u8( vandq_u8( vceqq_u8( v0, v1 ), vceqq_u8( v0, v2 ) ),
                                             vceqq_u8( v0, v3 ) );
                const auto runStartMask = equalByteMask( equal );
                if ( runStartMask == 0 ) {
                    vst1q_u8( reinterpret_cast<uint8_t*>( outputBuffer + nBytesDecoded ), v0 );
                    nBytesDecoded += 16;
                    state.position += 16;
                    continue;
                }

                const auto literalCount = static_cast<size_t>( __builtin_ctzll( runStartMask ) / 4 );
            #endif
                for ( size_t i = 0; i < literalCount; ++i ) {
                    outputBuffer[nBytesDecoded + i] = static_cast<char>( source[i] );
                }
                nBytesDecoded += literalCount;
                state.position += literalCount;
                state.repeatByte = source[literalCount];
                state.repeatCount = 4 + bwtData[state.position + 4];
                state.position += 5;
                handledRun = true;
                break;
            }
            if ( handledRun ) {
                continue;
            }
            /* The inner loop can fill the caller's buffer exactly. Recheck before the scalar path,
             * which would otherwise write one byte past nMaxBytesToDecode. */
            if ( ( nBytesDecoded >= nMaxBytesToDecode ) || ( state.position >= writeCount ) ) {
                break;
            }
        #endif

            const auto byte = bwtData[state.position];
            /* Four identical bytes are followed by a count of additional copies. */
            if ( ( state.position + 4 < writeCount )
                 && ( bwtData[state.position + 1] == byte )
                 && ( bwtData[state.position + 2] == byte )
                 && ( bwtData[state.position + 3] == byte ) ) {
                state.repeatByte = byte;
                state.repeatCount = 4 + bwtData[state.position + 4];
                state.position += 5;
            } else {
                outputBuffer[nBytesDecoded++] = static_cast<char>( byte );
                ++state.position;
            }
        }
    } else {
        const auto writeRepeatedSymbols =
            [&, this] ()
            {
                while ( ( symbolRepeatCount > 0 ) && ( nBytesDecoded < nMaxBytesToDecode ) ) {
                    --symbolRepeatCount;
                    outputBuffer[nBytesDecoded++] = static_cast<char>( symbolToRepeat );
                }
            };

        writeRepeatedSymbols();

        while ( ( writeCount > 0 ) && ( nBytesDecoded < nMaxBytesToDecode ) ) {
            writeCount--;

            // Follow sequence vector to undo Burrows-Wheeler transform.
            const auto previous = writeCurrent;
            writePos = dbuf[writePos];
            writeCurrent = static_cast<uint8_t>( writePos & 0xFFU );
            writePos >>= 8U;

            /* Whenever we see 3 consecutive copies of the same byte, the 4th is a repeat count */
            if ( writeRun < 3 ) {
                outputBuffer[nBytesDecoded++] = static_cast<char>( writeCurrent );
                if ( writeCurrent != previous ) {
                    writeRun = 0;
                } else {
                    ++writeRun;
                }
            } else {
                symbolToRepeat = previous;
                symbolRepeatCount = writeCurrent;
                writeRepeatedSymbols();
                writeCurrent = -1;
                writeRun = 0;
            }
        }
    }

    /* The bytes of this call are contiguous in outputBuffer, so the CRC can be computed in one slicing
     * pass after the run expansion instead of interleaved with it. */
    crc = updateCRC32( crc, outputBuffer, nBytesDecoded );
    dataCRC = crc;

    /* decompression of this block completed successfully */
    if ( ( inverseBWT && ( inverseBWT->position >= writeCount ) && ( inverseBWT->repeatCount == 0 ) )
         || ( !inverseBWT && ( writeCount == 0 ) && ( symbolRepeatCount == 0 ) ) ) {
        dataCRC = ~dataCRC;
        if ( dataCRC != headerCRC ) {
            std::stringstream msg;
            msg << "Calculated CRC " << std::hex << dataCRC << " for block mismatches " << headerCRC;
            throw std::runtime_error( std::move( msg ).str() );
        }
    }

    return nBytesDecoded;
}
}  // namespace bzip2
