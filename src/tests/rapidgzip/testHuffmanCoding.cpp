
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include <core/common.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/definitions.hpp>
#include <huffman/HuffmanCodingLinearSearch.hpp>
#include <huffman/HuffmanCodingShortBitsCached.hpp>
#include <huffman/HuffmanCodingSymbolsPerLength.hpp>
#include <rapidgzip/huffman/HuffmanCodingDoubleLiteralCached.hpp>
#include <rapidgzip/huffman/HuffmanCodingReversedBitsCached.hpp>
#include <rapidgzip/huffman/HuffmanCodingReversedBitsCachedCompressed.hpp>
#include <rapidgzip/huffman/HuffmanCodingReversedCodesPerLength.hpp>
#include <rapidgzip/huffman/HuffmanCodingReversedBitsCachedSeparateLengths.hpp>
#include <rapidgzip/huffman/HuffmanCodingShortBitsCachedDeflate.hpp>
#include <rapidgzip/huffman/HuffmanCodingShortBitsMultiCached.hpp>


using namespace rapidgzip;


template<typename HuffmanCoding>
void
decodeHuffmanAndCompare( const std::vector<uint8_t>&                        codeLengths,
                         const std::vector<uint8_t>&                        encoded,
                         const std::vector<typename HuffmanCoding::Symbol>& decoded )
{
    BufferedFileReader::AlignedBuffer encodedChars( encoded.size() );
    std::transform( encoded.begin(), encoded.end(), encodedChars.begin(), [] ( const auto c ) { return c; } );
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( encodedChars ) ) );

    HuffmanCoding coding;
    const auto errorCode = coding.initializeFromLengths( codeLengths );
    if ( errorCode != Error::NONE ) {
        std::cerr << "Could not create HuffmanCoding from given lengths because: " << toString( errorCode ) << "\n";
        REQUIRE( false );
        return;
    }

    for ( const auto symbol : decoded ) {
        const auto decodedSymbol = coding.decode( bitReader );
        REQUIRE( decodedSymbol.has_value() );

        if ( decodedSymbol && ( decodedSymbol != symbol ) ) {
            std::cerr << "Decoded symbol " << (int)*decodedSymbol << " mismatches expected " << (int)symbol << "\n";
        }
        REQUIRE( decodedSymbol == symbol );
    }
    REQUIRE( ceilDiv( bitReader.tell(), CHAR_BIT ) * CHAR_BIT == bitReader.size() );
}


template<typename HuffmanCoding>
void
testHuffmanCodingInvalidDetection()
{
    const std::vector<char> encoded = { 0b0110'1110 };
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( encoded ) );

    HuffmanCoding coding;
    const std::vector<uint8_t> codeLengthsHalfBit = { 1 };
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengthsHalfBit ), rapidgzip::Error::NONE );

    REQUIRE( coding.decode( bitReader ).has_value() );
    REQUIRE( !coding.decode( bitReader ).has_value() );
}


template<typename HuffmanCoding>
void
testHuffmanCodingReuse( bool testOneSymbolCoding = true )
{
    const std::vector<char> encoded = { 0b0110'1101 };
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( encoded ) );

    const std::vector<uint8_t> codeLengths2Bit = { 2, 2, 2, 2 };
    HuffmanCoding coding;
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths2Bit ), rapidgzip::Error::NONE );

    /* Note that gzip Huffman decoding iterates over bits from the least significant first, meaning that the
     * second symbol has bit sequence 0b01 (reverse read 0b10 = 2). */
    REQUIRE_EQUAL( coding.decode( bitReader ).value(), 2 );

    const std::vector<uint8_t> codeLengths1Bit = { 1, 1 };
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths1Bit ), rapidgzip::Error::NONE );
    bitReader.seek( 2 );
    /* When not reinitializing the cached next symbol, this might return symbols that are not even value,
     * e.g., it will return 3 even though only 0 and 1 are possible! */
    REQUIRE_EQUAL( coding.decode( bitReader ).value(), 1 );

    /* Ensure that caches and such are correctly cleared so that invalid bit sequences will be detected correctly. */
    if ( testOneSymbolCoding ) {
        const std::vector<uint8_t> codeLengthsHalfBit = { 1 };
        REQUIRE_EQUAL( coding.initializeFromLengths( codeLengthsHalfBit ), rapidgzip::Error::NONE );
        bitReader.seek( 0 );
        REQUIRE( !coding.decode( bitReader ).has_value() );
    }
}


template<typename HuffmanCoding>
void
testHuffmanCoding( bool testOneSymbolCoding = true )
{
    if ( testOneSymbolCoding ) {
        testHuffmanCodingInvalidDetection<HuffmanCoding>();
    }
    testHuffmanCodingReuse<HuffmanCoding>( testOneSymbolCoding );

    if ( testOneSymbolCoding ) {
        /* A single symbol with code length 1 should also be a valid Huffman Coding. */
        decodeHuffmanAndCompare<HuffmanCoding>( /* code lengths */ { 1 }, /* encoded */ { 0 }, /* decoded */ { 0 } );
    }

    /* codeLengths, encoded, decoded */
    decodeHuffmanAndCompare<HuffmanCoding>( { 1, 1 }, { 0 }, { 0 } );
    decodeHuffmanAndCompare<HuffmanCoding>( { 1, 1 }, { 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 } );
    decodeHuffmanAndCompare<HuffmanCoding>( { 1, 1 }, { 0b1010 }, { 0, 1, 0, 1, 0, 0, 0, 0 } );

    decodeHuffmanAndCompare<HuffmanCoding>( { 1, 2, 2 }, { 0b11'01'0 }, { 0, 1, 2, 0, 0 } );
    decodeHuffmanAndCompare<HuffmanCoding>( { 1, 3, 3, 3, 3 }, { 0b111'001'0 }, { 0, 1, 4, 0 } );
    decodeHuffmanAndCompare<HuffmanCoding>( { 1, 3, 3, 3, 3 }, { 0b011'101'0 }, { 0, 2, 3, 0 } );

    /* Code length 8 can be easily "encoded" for the tests because no bit shifting is necessary because each
     * byte maps to an encoded byte. */
    if ( HuffmanCoding::MAX_CODE_LENGTH >= 8U ) {
        constexpr auto CODE_LENGTH = 8U;
        const std::vector<typename HuffmanCoding::BitCount> equalCodeLengths( 1U << CODE_LENGTH, CODE_LENGTH );
        std::vector<typename HuffmanCoding::Symbol> decoded( equalCodeLengths.size() );
        std::vector<uint8_t> encoded( equalCodeLengths.size() );
        for ( size_t i = 0; i < encoded.size(); ++i ) {
            decoded[i] = i;
            encoded[i] = REVERSED_BITS_LUT<uint8_t>.at( static_cast<uint8_t>( i ) );
        }
        decodeHuffmanAndCompare<HuffmanCoding>( equalCodeLengths, encoded, decoded );
    }
}


template<typename HuffmanCoding>
void
testEmptyAlphabet()
{
    /* Deflate allows an empty distance alphabet for blocks without back-references. All code lengths are zero,
     * so there is no symbol to decode and the decoder must not peek zero bits. */
    const std::vector<uint8_t> codeLengths( 1, 0 );
    HuffmanCoding coding;
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths ), rapidgzip::Error::NONE );

    const std::vector<char> encoded = { 0 };
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( encoded ) );
    REQUIRE( !coding.decode( bitReader ).has_value() );
}


void
testEmptyAlphabetMultiCached()
{
    /* Same as above, but this decoder returns a packed symbol count instead of an optional. A symbol count of zero
     * signals an invalid code to the caller. */
    using HuffmanCoding = rapidgzip::deflate::HuffmanCodingShortBitsMultiCached<11>;
    const std::vector<uint8_t> codeLengths( 1, 0 );
    HuffmanCoding coding;
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths ), rapidgzip::Error::NONE );

    const std::vector<char> encoded = { 0 };
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( encoded ) );
    const auto [symbol, symbolCount] = coding.decode( bitReader );
    REQUIRE_EQUAL( symbolCount, 0U );
    REQUIRE_EQUAL( symbol, 0U );
}


void
testEmptyAlphabetDeflateSpecific()
{
    /* This decoder has a fixed-size LUT peek, so an empty alphabet does not peek zero bits. But with fewer bits
     * available than the LUT size, the EOF fallback used to dereference an empty optional. */
    using HuffmanCoding = rapidgzip::deflate::HuffmanCodingShortBitsCachedDeflate<11>;
    using DistanceHuffmanCoding =
        HuffmanCodingReversedBitsCached<uint16_t, 15, uint8_t, rapidgzip::deflate::MAX_DISTANCE_SYMBOL_COUNT>;
    const std::vector<uint8_t> codeLengths( 1, 0 );
    HuffmanCoding coding;
    const DistanceHuffmanCoding distanceCoding;
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths, distanceCoding ), rapidgzip::Error::NONE );

    const std::vector<char> encoded = { 0 };
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( encoded ) );

    bool threwInvalidCode{ false };
    try {
        (void)coding.decode( bitReader, distanceCoding );
    } catch ( const rapidgzip::Error& error ) {
        threwInvalidCode = error == rapidgzip::Error::INVALID_HUFFMAN_CODE;
    } catch ( const std::exception& ) {}
    REQUIRE( threwInvalidCode );
}


void
testInvalidCodeAtEndOfInputDeflateSpecific()
{
    /* Fewer input bits than the LUT size and an invalid code used to reach the EOF fallback, which dereferenced
     * the empty optional returned by the bit-by-bit decoder. */
    using HuffmanCoding = rapidgzip::deflate::HuffmanCodingShortBitsCachedDeflate<11>;
    using DistanceHuffmanCoding =
        HuffmanCodingReversedBitsCached<uint16_t, 15, uint8_t, rapidgzip::deflate::MAX_DISTANCE_SYMBOL_COUNT>;
    const std::vector<uint8_t> codeLengths( 1, 1 );
    HuffmanCoding coding;
    const DistanceHuffmanCoding distanceCoding;
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths, distanceCoding ), rapidgzip::Error::NONE );

    /* The only symbol has code 0b0 and the first bit is 1. With only 8 input bits, the 11-bit LUT peek fails
     * and the fallback bit-by-bit decoder must reject the invalid code instead of returning an empty optional. */
    const std::vector<char> encoded = { 1 };
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( encoded ) );

    bool threwInvalidCode{ false };
    try {
        (void)coding.decode( bitReader, distanceCoding );
    } catch ( const rapidgzip::Error& error ) {
        threwInvalidCode = error == rapidgzip::Error::INVALID_HUFFMAN_CODE;
    } catch ( const std::exception& ) {}
    REQUIRE( threwInvalidCode );
}


void
testSeparateLengthsMaxSymbolCount()
{
    /* Using exactly MAX_SYMBOL_COUNT code lengths used to write one past m_codeLengths. */
    using HuffmanCoding = HuffmanCodingReversedBitsCachedSeparateLengths<uint16_t, 15, uint16_t, 4>;
    const std::vector<uint8_t> codeLengths{ 1, 2, 2, 0 };
    HuffmanCoding coding;
    REQUIRE_EQUAL( coding.initializeFromLengths( codeLengths ), rapidgzip::Error::NONE );
}


int
main()
{
    testSeparateLengthsMaxSymbolCount();
    static constexpr auto MAX_CODE_LENGTH = 15;
    static constexpr auto MAX_SYMBOL_COUNT = 512;

    std::cerr << "Testing empty alphabets...\n";
    testEmptyAlphabet<HuffmanCodingShortBitsCached<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_SYMBOL_COUNT, 11, true> >();
    testEmptyAlphabet<HuffmanCodingShortBitsCached<uint32_t, 20, uint16_t, 258, 12, false> >();
    testEmptyAlphabet<HuffmanCodingReversedBitsCached<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_SYMBOL_COUNT> >();
    testEmptyAlphabet<HuffmanCodingReversedBitsCachedCompressed<uint16_t, MAX_CODE_LENGTH,
                                                                uint16_t, MAX_SYMBOL_COUNT> >();
    testEmptyAlphabet<HuffmanCodingReversedBitsCachedSeparateLengths<uint16_t, MAX_CODE_LENGTH,
                                                                     uint16_t, MAX_SYMBOL_COUNT> >();
    testEmptyAlphabetMultiCached();
    testEmptyAlphabetDeflateSpecific();
    testInvalidCodeAtEndOfInputDeflateSpecific();

    std::cerr << "Testing HuffmanCodingLinearSearch...\n";
    testHuffmanCoding<HuffmanCodingLinearSearch<uint16_t, uint16_t> >();

    std::cerr << "Testing HuffmanCodingSymbolsPerLength...\n";
    testHuffmanCoding<HuffmanCodingSymbolsPerLength<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_SYMBOL_COUNT> >();

    std::cerr << "Testing HuffmanCodingReversedBitsCached...\n";
    testHuffmanCoding<HuffmanCodingReversedBitsCached<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_SYMBOL_COUNT> >();

    std::cerr << "Testing HuffmanCodingReversedBitsCachedCompressed...\n";
    testHuffmanCoding<HuffmanCodingReversedBitsCachedCompressed<uint16_t, MAX_CODE_LENGTH,
                                                                uint16_t, MAX_SYMBOL_COUNT> >();

    std::cerr << "Testing HuffmanCodingReversedCodesPerLength...\n";
    testHuffmanCoding<HuffmanCodingReversedCodesPerLength<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_SYMBOL_COUNT> >();

    std::cerr << "Testing HuffmanCodingReversedCodesPerLength with precode configuration...\n";
    using namespace rapidgzip::deflate;
    testHuffmanCoding<HuffmanCodingReversedCodesPerLength<uint16_t, MAX_PRECODE_LENGTH, uint8_t, MAX_PRECODE_COUNT> >();
    testHuffmanCoding<HuffmanCodingReversedCodesPerLength<uint8_t, MAX_PRECODE_LENGTH, uint8_t, MAX_PRECODE_COUNT> >();

    std::cerr << "Testing HuffmanCodingDoubleLiteralCached...\n";
    testHuffmanCoding<HuffmanCodingDoubleLiteralCached<uint16_t, MAX_CODE_LENGTH, uint16_t, MAX_SYMBOL_COUNT> >(
        /* do not test one-symbol codings because this implementation does not support it */ false );

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
