#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <core/common.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Buffered.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/definitions.hpp>
#include <rapidgzip/gzip/GzipAnalyzer.hpp>
#include <rapidgzip/gzip/deflate.hpp>


using namespace rapidgzip;


void
testCalculateDistance()
{
    using namespace deflate;

    REQUIRE( calculateDistance( 4, 1, 0b0 ) == 5 );
    REQUIRE( calculateDistance( 4, 1, 0b1 ) == 6 );
    REQUIRE( calculateDistance( 5, 1, 0b0 ) == 7 );
    REQUIRE( calculateDistance( 5, 1, 0b1 ) == 8 );
    REQUIRE( calculateDistance( 6, 2, 0b00 ) == 9 );
    REQUIRE( calculateDistance( 6, 2, 0b01 ) == 10 );
    REQUIRE( calculateDistance( 6, 2, 0b10 ) == 11 );
    REQUIRE( calculateDistance( 6, 2, 0b11 ) == 12 );

    REQUIRE( calculateDistance( 7, 2, 0b00 ) == 13 );
    REQUIRE( calculateDistance( 7, 2, 0b01 ) == 14 );
    REQUIRE( calculateDistance( 7, 2, 0b10 ) == 15 );
    REQUIRE( calculateDistance( 7, 2, 0b11 ) == 16 );
    REQUIRE( calculateDistance( 8, 3, 0b000 ) == 17 );
    REQUIRE( calculateDistance( 8, 3, 0b001 ) == 18 );
    REQUIRE( calculateDistance( 8, 3, 0b010 ) == 19 );
    REQUIRE( calculateDistance( 8, 3, 0b011 ) == 20 );

    REQUIRE( calculateDistance( 19, 8, 0xFA ) == 769 + 0xFA );

    /* These first 4 do not require extra bits and therefore are treated specially anyway. */
    //REQUIRE( calculateDistance( 0  ) == 1 );
    //REQUIRE( calculateDistance( 1  ) == 2 );  // returns 1 instead of 2! But unused anyway ...
    //REQUIRE( calculateDistance( 2  ) == 3 );
    //REQUIRE( calculateDistance( 3  ) == 4 );
    REQUIRE( calculateDistance( 4 ) == 5 );
    REQUIRE( calculateDistance( 5 ) == 7 );
    REQUIRE( calculateDistance( 6 ) == 9 );
    REQUIRE( calculateDistance( 7 ) == 13 );
    REQUIRE( calculateDistance( 8 ) == 17 );
    REQUIRE( calculateDistance( 9 ) == 25 );
    REQUIRE( calculateDistance( 10 ) == 33 );
    REQUIRE( calculateDistance( 11 ) == 49 );

    REQUIRE( calculateDistance( 19 ) == 769 );
    REQUIRE( calculateDistance( 25 ) == 6145 );
    REQUIRE( calculateDistance( 29 ) == 24577 );
}


void
testCalculateLength()
{
    using namespace deflate;

    /* These 4 do not require special bits and therefore are specially treated anyway. */
    // *INDENT-OFF*
    REQUIRE( calculateLength( 261 - 261 ) == 7   );
    REQUIRE( calculateLength( 262 - 261 ) == 8   );
    REQUIRE( calculateLength( 263 - 261 ) == 9   );
    REQUIRE( calculateLength( 264 - 261 ) == 10  );

    REQUIRE( calculateLength( 265 - 261 ) == 11  );
    REQUIRE( calculateLength( 266 - 261 ) == 13  );
    REQUIRE( calculateLength( 267 - 261 ) == 15  );
    REQUIRE( calculateLength( 268 - 261 ) == 17  );

    REQUIRE( calculateLength( 269 - 261 ) == 19  );
    REQUIRE( calculateLength( 270 - 261 ) == 23  );
    REQUIRE( calculateLength( 271 - 261 ) == 27  );
    REQUIRE( calculateLength( 272 - 261 ) == 31  );

    REQUIRE( calculateLength( 273 - 261 ) == 35  );
    REQUIRE( calculateLength( 274 - 261 ) == 43  );
    REQUIRE( calculateLength( 275 - 261 ) == 51  );
    REQUIRE( calculateLength( 276 - 261 ) == 59  );

    REQUIRE( calculateLength( 277 - 261 ) == 67  );
    REQUIRE( calculateLength( 278 - 261 ) == 83  );
    REQUIRE( calculateLength( 279 - 261 ) == 99  );
    REQUIRE( calculateLength( 280 - 261 ) == 115 );

    REQUIRE( calculateLength( 281 - 261 ) == 131 );
    REQUIRE( calculateLength( 282 - 261 ) == 163 );
    REQUIRE( calculateLength( 283 - 261 ) == 195 );
    REQUIRE( calculateLength( 284 - 261 ) == 227 );
    // *INDENT-ON*
}


template<typename HuffmanCoding>
void
decodeHuffmanAndCompare( const HuffmanCoding&                               coding,
                         const std::vector<uint8_t>&                        encoded,
                         const std::vector<typename HuffmanCoding::Symbol>& decoded )
{
    BufferedFileReader::AlignedBuffer encodedChars( encoded.size() );
    std::transform( encoded.begin(), encoded.end(), encodedChars.begin(), [] ( const auto c ) { return c; } );
    gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( std::move( encodedChars ) ) );

    for ( const auto symbol : decoded ) {
        const auto decodedSymbol = coding.decode( bitReader );
        REQUIRE( decodedSymbol.has_value() );

        if ( decodedSymbol && ( decodedSymbol != symbol ) ) {
            std::cerr << "Decoded symbol " << (int)*decodedSymbol << " mismatches expected " << (int)symbol << "\n";
        }
        REQUIRE( decodedSymbol == symbol );
    }
    REQUIRE_EQUAL( ceilDiv( bitReader.tell(), CHAR_BIT ) * CHAR_BIT, bitReader.size().value() );
}


void
testFixedHuffmanCoding()
{
    /**
     * @verbatim
     *  Lit Value  Bits    Codes
     * ---------   ----    -----
     * 0 - 143     8       00110000 through = 24 (bit length 7 count!)
     *                     10111111
     * 144 - 255   9       110010000 through
     *                     111111111
     * 256 - 279   7       0000000 through
     *                     0010111 = 23
     * 280 - 287   8       11000000 through
     *                     11000111
     * @endverbatim
     */
    const auto fixedHuffmanCoding = deflate::createFixedHC();

    std::vector<uint8_t> encoded = { 0 };
    std::vector<uint16_t> decoded = { 256 };
    decodeHuffmanAndCompare( fixedHuffmanCoding, encoded, decoded );

    /* Test 8-bit codes. */
    encoded.clear();
    decoded.clear();
    for ( size_t i = 0; i < 144; ++i ) {
        encoded.push_back( reverseBits( static_cast<uint8_t>( 0b0011'0000UL + i ) ) );
        decoded.push_back( i );
    }
    for ( size_t i = 0; i < 8; ++i ) {
        encoded.push_back( reverseBits( static_cast<uint8_t>( 0b1100'0000 + i ) ) );
        decoded.push_back( 280 + i );
    }
    decodeHuffmanAndCompare( fixedHuffmanCoding, encoded, decoded );

    /* Test 7-bit codes */
    for ( size_t i = 0; i < 279 - 256 + 1; ++i ) {
        encoded = { static_cast<uint8_t>( reverseBits( static_cast<uint8_t>( i ) ) >> 1U ) };
        decoded = { static_cast<uint16_t>( 256 + i ) };
        decodeHuffmanAndCompare( fixedHuffmanCoding, encoded, decoded );
    }

    /* Test 9-bit codes */
    for ( size_t i = 0; i < 255 - 144 + 1; ++i ) {
        const auto code = static_cast<uint32_t>(
            reverseBits( static_cast<uint16_t>( 0b1'1001'0000UL + i ) ) >> ( 16U - 9U ) );
        encoded = { static_cast<uint8_t>( code & 0xFFU ), static_cast<uint8_t>( ( code >> 8U ) & 0xFFU ) };

        BufferedFileReader::AlignedBuffer encodedChars( encoded.size() );
        std::transform( encoded.begin(), encoded.end(), encodedChars.begin(), [] ( const auto c ) { return c; } );
        gzip::BitReader bitReader( std::make_unique<BufferedFileReader>( encodedChars ) );
        REQUIRE( bitReader.read<9>() == code );

        decoded = { static_cast<uint16_t>( 144 + i ) };
        decodeHuffmanAndCompare( fixedHuffmanCoding, encoded, decoded );
    }

    /* Test 7-bit and 9-bit codes */
    encoded.clear();
    decoded.clear();
    for ( size_t i = 0; i < 255 - 144 + 1; ++i ) {
        constexpr auto literals7BitsCount = 279 - 256 + 1;
        const auto mergedCode = reverseBits(
            static_cast<uint16_t>(
                ( /* 7-bit */ ( i % literals7BitsCount ) << 9U ) | /* 9-bit */ ( 0b1'1001'0000UL + i )
        ) );

        encoded.push_back( mergedCode & nLowestBitsSet<uint16_t, 8>() );
        encoded.push_back( static_cast<uint8_t>( static_cast<uint32_t>( mergedCode ) >> 8U ) );
        decoded.push_back( 256 + ( i % literals7BitsCount ) );
        decoded.push_back( 144 + i );
    }
    decodeHuffmanAndCompare( fixedHuffmanCoding, encoded, decoded );
}


void
testTruncatedStoredBlock()
{
    /* Raw DEFLATE: BFINAL=1, BTYPE=stored, LEN=64, NLEN=~64, but no payload. */
    const std::vector<uint8_t> truncatedData{ 0x01, 0x40, 0x00, 0xBF, 0xFF };
    for ( const auto readSize : { 1, 63, /* internal buffer size for cache line performance */ 64, 65 } ) {
        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( truncatedData ) );
        deflate::Block<false> block;
        REQUIRE( block.readHeader( bitReader ) == Error::NONE );

        /* Previously, view.size() was always 64, the internal buffer size, and Error::NONE!
         * This effectively would return wrong data to a user. */
        const auto [view, error] = block.read( bitReader, readSize );
        REQUIRE( error == Error::EOF_UNCOMPRESSED );
        REQUIRE_EQUAL( view.size(), 0U );

        /* A partially present payload must only return the bytes that were actually read. */
        auto partiallyTruncatedData = truncatedData;
        partiallyTruncatedData.insert( partiallyTruncatedData.end(), { 'a', 'b', 'c' } );
        gzip::BitReader partialBitReader( std::make_unique<BufferViewFileReader>( partiallyTruncatedData ) );
        deflate::Block<false> partialBlock;
        REQUIRE( partialBlock.readHeader( partialBitReader ) == Error::NONE );

        /* Previously, view.size() was always 64, the internal buffer size, and Error::NONE! */
        const auto [partialView, partialError] = partialBlock.read( partialBitReader, readSize );
        REQUIRE( partialError == Error::EOF_UNCOMPRESSED );
        REQUIRE_EQUAL( partialView.size(), 3U );
    }

    /* LEN=100 with only 90 payload bytes exercises the remainder after the 64-byte bulk loop. */
    std::vector<uint8_t> nonMultipleOf64{ 0x01, 100, 0x00, 0x9B, 0xFF };
    nonMultipleOf64.insert( nonMultipleOf64.end(), 90, 'x' );
    gzip::BitReader remainderBitReader( std::make_unique<BufferViewFileReader>( nonMultipleOf64 ) );
    deflate::Block<false> remainderBlock;
    REQUIRE( remainderBlock.readHeader( remainderBitReader ) == Error::NONE );

    /* Previously, this threw rapidgzip::BitReader<false, unsigned long>::EndOfFileReached. But then why do we
     * even have an error argument being returned? Instead, it should convert the exception into an error. */
    const auto [remainderView, remainderError] = remainderBlock.read( remainderBitReader, 100 );
    REQUIRE( remainderError == Error::EOF_UNCOMPRESSED );
    REQUIRE_EQUAL( remainderView.size(), 90U );
    for ( const auto& view : remainderView.data ) {
        REQUIRE( std::all_of( view.begin(), view.end(), [] ( const auto c ) { return c == 'x'; } ) );
    }
}


void
testAnalyzeTruncatedGzip()
{
    const auto analyzeBytes =
        [] ( const std::vector<uint8_t>& bytes ) {
            BufferedFileReader::AlignedBuffer buffer( bytes.size() );
            std::copy( bytes.begin(), bytes.end(), buffer.begin() );
            return rapidgzip::deflate::analyze(
                std::make_unique<BufferedFileReader>( std::move( buffer ) ) );
        };

    const std::vector<uint8_t> gzipHeader = {
        0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    };

    /* Truncated stored-block LEN/NLEN header. */
    auto truncatedHeader = gzipHeader;
    truncatedHeader.push_back( 0x00 );
    REQUIRE( analyzeBytes( truncatedHeader ) != rapidgzip::Error::NONE );

    /* Non-final and final stored blocks with LEN=4 but only two data bytes. The analyzer used to
     * continue after the decode error and report success for the truncated non-final stream. */
    for ( const auto finalBlockByte : { uint8_t( 0x00 ), uint8_t( 0x01 ) } ) {
        auto truncatedBody = gzipHeader;
        const std::vector<uint8_t> blockHeader{ finalBlockByte, 0x04, 0x00, 0xFB, 0xFF, 'a', 'b' };
        truncatedBody.insert( truncatedBody.end(), blockHeader.begin(), blockHeader.end() );
        REQUIRE( analyzeBytes( truncatedBody ) != rapidgzip::Error::NONE );
    }
}


void
testInvalidFixedHuffmanLengthSymbols()
{
    /* Fixed Huffman codes 286 and 287 are deflate-invalid length symbols, but they have valid 8-bit codes in the
     * fixed tree. A malformed stream must be rejected with INVALID_HUFFMAN_CODE instead of being decoded as a
     * back-reference length. The bytes are BFINAL=1, BTYPE=fixed followed by the reversed 8-bit code of 286 or 287. */
    for ( const auto& data : { std::vector<uint8_t>{ 0x1B, 0x03 }, /* 286 */
                               std::vector<uint8_t>{ 0x1B, 0x07 } /* 287 */ } )
    {
        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( data ) );
        deflate::Block<false> block;
        REQUIRE( block.readHeader( bitReader ) == Error::NONE );
        const auto [view, error] = block.read( bitReader, 1024 );
        REQUIRE( error == Error::INVALID_HUFFMAN_CODE );
        REQUIRE_EQUAL( view.size(), 0U );
    }
}


void
testInvalidDynamicHuffmanCodeCount()
{
    /* codeLengthCount is HLIT + 257. HLIT=29 means 286 literal/length symbols, the maximum that deflate defines.
     * The truncated header then ends while reading the rest of the dynamic header, which is not an exceeded range. */
    {
        const std::vector<uint8_t> data{ 0xED };
        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( data ) );
        deflate::Block<false> block;
        REQUIRE( block.readHeader( bitReader ) == Error::END_OF_FILE );
    }

    /* HLIT=30 means 287 literal/length symbols and must be rejected before any code lengths are read. */
    {
        const std::vector<uint8_t> data{ 0xF5 };
        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( data ) );
        deflate::Block<false> block;
        REQUIRE( block.readHeader( bitReader ) == Error::EXCEEDED_LITERAL_RANGE );
    }
}


void
testInvalidDeflateErrors()
{
    /* BFINAL=1, BTYPE=3 (reserved). readHeader sets the compression type and returns
     * INVALID_COMPRESSION. read must return the same error instead of throwing std::domain_error. */
    {
        const std::vector<uint8_t> data{ 0b00000111 };
        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( data ) );
        deflate::Block<false> block;
        REQUIRE( block.readHeader( bitReader ) == Error::INVALID_COMPRESSION );
        REQUIRE( block.read( bitReader, 1024 ).second == Error::INVALID_COMPRESSION );
    }

    /* A fixed Huffman block with length symbol 257 and reserved distance symbol 30. Decoding the length
     * symbol then reads the reserved distance code, which must return EXCEEDED_DISTANCE_RANGE instead of
     * throwing std::logic_error. */
    {
        const std::vector<uint8_t> data{ 3, 62 };
        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( data ) );
        deflate::Block<false> block;
        REQUIRE( block.readHeader( bitReader ) == Error::NONE );
        const auto [view, error] = block.read( bitReader, 1024 );
        REQUIRE( error == Error::EXCEEDED_DISTANCE_RANGE );
        REQUIRE( view.size() == 0U );
    }
}



namespace
{
/**
 * Minimal LSB-first deflate bit writer. Huffman codes are transmitted most-significant bit first, extra bits
 * and header fields least-significant bit first.
 */
class DeflateBitWriter
{
public:
    void
    writeBits( uint32_t value,
               uint8_t  count )
    {
        m_bitBuffer |= value << m_bitCount;
        m_bitCount += count;
        while ( m_bitCount >= 8 ) {
            m_data.push_back( static_cast<uint8_t>( m_bitBuffer & 0xFFU ) );
            m_bitBuffer >>= 8U;
            m_bitCount -= 8;
        }
    }

    void
    writeCode( uint32_t code,
               uint8_t  count )
    {
        uint32_t reversed{ 0 };
        for ( uint8_t i = 0; i < count; ++i ) {
            reversed |= ( ( code >> ( count - 1 - i ) ) & 1U ) << i;
        }
        writeBits( reversed, count );
    }

    void
    alignToByte()
    {
        if ( ( m_bitCount % 8 ) != 0 ) {
            writeBits( 0, static_cast<uint8_t>( 8 - ( m_bitCount % 8 ) ) );
        }
    }

    [[nodiscard]] std::vector<uint8_t>
    finish()
    {
        if ( m_bitCount > 0 ) {
            m_data.push_back( static_cast<uint8_t>( m_bitBuffer & 0xFFU ) );
            m_bitBuffer = 0;
            m_bitCount = 0;
        }
        return m_data;
    }

private:
    std::vector<uint8_t> m_data;
    uint32_t m_bitBuffer{ 0 };
    uint8_t m_bitCount{ 0 };
};


void
writeFixedLiteralOrLength( DeflateBitWriter& writer,
                           const uint16_t    symbol )
{
    if ( symbol <= 143 ) {
        writer.writeCode( 0x30U + symbol, 8 );
    } else if ( symbol <= 255 ) {
        writer.writeCode( 0x190U + ( symbol - 144 ), 9 );
    } else if ( symbol <= 279 ) {
        writer.writeCode( symbol - 256, 7 );
    } else {
        writer.writeCode( 0xC0U + ( symbol - 280 ), 8 );
    }
}


void
writeFixedDistance( DeflateBitWriter& writer,
                    const uint16_t    symbol,
                    const uint32_t    extraBits,
                    const uint8_t     extraBitCount )
{
    writer.writeCode( symbol, 5 );
    if ( extraBitCount > 0 ) {
        writer.writeBits( extraBits, extraBitCount );
    }
}


void
writeStoredBlock( DeflateBitWriter&           writer,
                  const std::vector<uint8_t>& data,
                  const bool                  isFinal )
{
    writer.writeBits( isFinal ? 1U : 0U, 1 );
    writer.writeBits( 0, 2 );  /* BTYPE = stored */
    writer.alignToByte();
    const auto size = static_cast<uint16_t>( data.size() );
    writer.writeBits( size, 16 );
    writer.writeBits( static_cast<uint16_t>( ~size ), 16 );
    for ( const auto byte : data ) {
        writer.writeBits( byte, 8 );
    }
}
}  // namespace


/**
 * Decode a fixed-Huffman block that immediately references the unknown window and contains overlapping
 * matches (distance < length), a distance-1 match, and a short overlapping match. Starting with a fresh Block
 * makes those references markers. The marker values must survive the copy paths unchanged and must still map
 * through the window to the referenced bytes.
 */
void
testMarkerBackreferenceCopies()
{
    DeflateBitWriter writer;
    writer.writeBits( 1, 1 );  /* BFINAL */
    writer.writeBits( 1, 2 );  /* BTYPE = fixed Huffman */

    /* 258 symbols from the unknown window at distance 1000. The source and destination do not overlap,
     * so this takes the disjoint memcpy path and the markers stay unchanged. */
    writeFixedLiteralOrLength( writer, 285 );
    writeFixedDistance( writer, 19, 231, 8 );

    /* Distance 1: the first symbol comes from an unknown window and is repeated by the fill path. */
    writeFixedLiteralOrLength( writer, 285 );
    writeFixedDistance( writer, 0, 0, 0 );

    /* Overlapping match with distance 3. */
    writeFixedLiteralOrLength( writer, 285 );
    writeFixedDistance( writer, 2, 0, 0 );

    /* A literal followed by a match whose period contains a marker and a literal. */
    writeFixedLiteralOrLength( writer, 'B' );
    writeFixedLiteralOrLength( writer, 285 );
    writeFixedDistance( writer, 2, 0, 0 );

    /* Short overlapping match with distance 2. */
    writeFixedLiteralOrLength( writer, 264 );  /* length 10 */
    writeFixedDistance( writer, 1, 0, 0 );

    writeFixedLiteralOrLength( writer, 256 );  /* end of block */

    constexpr size_t EXPECTED_SIZE = 3 * 258 + 1 + 258 + 10;
    static constexpr size_t DISTANCE_1000_MARKER = size_t( 2 ) * deflate::MAX_WINDOW_SIZE - 1000;

    auto compressed = writer.finish();
    /* The grouped Huffman decoder peeks ahead, so keep enough trailing bytes for it to finish the group. */
    compressed.insert( compressed.end(), 8, 0 );
    gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( compressed ) );
    auto block = std::make_unique<deflate::Block<true> >();
    REQUIRE( block->readHeader( bitReader ) == Error::NONE );
    const auto [view, error] = block->read( bitReader, EXPECTED_SIZE + deflate::MAX_RUN_LENGTH );
    REQUIRE( error == Error::NONE );
    REQUIRE( block->eob() );
    REQUIRE_EQUAL( view.size(), EXPECTED_SIZE );

    /* Pin the exercised copy paths so that a future change cannot silently fall back to the per-symbol loop. */
    REQUIRE_EQUAL( block->markerBackreferences.disjointMemcpy, 1U );
    REQUIRE_EQUAL( block->markerBackreferences.distanceOneFill, 1U );
    REQUIRE_EQUAL( block->markerBackreferences.repeatedMemcpy, 3U );
    REQUIRE_EQUAL( block->markerBackreferences.perSymbol, 0U );
    REQUIRE_EQUAL( view.dataWithMarkersSize(), EXPECTED_SIZE );

    std::vector<uint16_t> symbols;
    for ( const auto& buffer : view.dataWithMarkers ) {
        symbols.insert( symbols.end(), buffer.begin(), buffer.end() );
    }
    REQUIRE_EQUAL( symbols.size(), EXPECTED_SIZE );

    /* The first 258 symbols are consecutive markers from the unknown window. */
    for ( size_t i = 0; i < 258; ++i ) {
        if ( symbols[i] != DISTANCE_1000_MARKER + i ) {
            std::cerr << "Unexpected marker at " << i << ": " << symbols[i] << "\n";
            REQUIRE( symbols[i] == DISTANCE_1000_MARKER + i );
        }
    }
    const auto firstMarker = symbols.front();
    const auto markerAfterDistanceOne = symbols[258];
    for ( size_t i = 258; i < 774; ++i ) {
        REQUIRE( symbols[i] == markerAfterDistanceOne );
    }
    REQUIRE( symbols[774] == 'B' );
    for ( size_t i = 775; i < 1033; ++i ) {
        const auto expected = ( i - 775 ) % 3 == 2 ? uint16_t( 'B' ) : markerAfterDistanceOne;
        REQUIRE( symbols[i] == expected );
    }
    for ( size_t i = 1033; i < EXPECTED_SIZE; ++i ) {
        const auto expected = ( i - 1033 ) % 2 == 1 ? uint16_t( 'B' ) : markerAfterDistanceOne;
        REQUIRE( symbols[i] == expected );
    }

    /* The marker values must still resolve through the window to the referenced bytes. */
    std::vector<uint8_t> window( deflate::MAX_WINDOW_SIZE );
    for ( size_t i = 0; i < window.size(); ++i ) {
        window[i] = static_cast<uint8_t>( ( i * 7 + 3 ) & 0xFFU );
    }
    const auto expectedByte =
        [&window] ( const uint16_t symbol )
        {
            return symbol < 256 ? static_cast<uint8_t>( symbol ) : window[symbol - deflate::MAX_WINDOW_SIZE];
        };
    std::transform( symbols.begin(), symbols.end(), symbols.begin(),
                    deflate::MapMarkers<true>( window ) );
    for ( size_t i = 0; i < symbols.size(); ++i ) {
        const auto expected =
            [&] () {
                /* Recomputed from the marker sequence asserted above. */
                if ( i < 258 ) {
                    return window[firstMarker - deflate::MAX_WINDOW_SIZE + i];
                }
                if ( i < 774 ) {
                    return expectedByte( markerAfterDistanceOne );
                }
                if ( i == 774 ) {
                    return uint8_t( 'B' );
                }
                if ( i < 1033 ) {
                    return ( i - 775 ) % 3 == 2
                           ? uint8_t( 'B' )
                           : window[markerAfterDistanceOne - deflate::MAX_WINDOW_SIZE];
                }
                return ( i - 1033 ) % 2 == 1
                       ? uint8_t( 'B' )
                       : window[markerAfterDistanceOne - deflate::MAX_WINDOW_SIZE];
            }();
        if ( symbols[i] != expected ) {
            std::cerr << "Unexpected mapped byte at " << i << ": " << int( symbols[i] ) << "\n";
            REQUIRE( symbols[i] == expected );
        }
    }
}


/**
 * Cover the three stored-block fast paths in Block::read and the readInternalUncompressed fallback. The
 * marker case also appends a back-reference at distance MAX_WINDOW_SIZE to verify that the recent suffix and
 * the stored block are concatenated in the correct order for the following block, which a fallback-only run
 * also has to satisfy.
 */
void
testStoredBlockFastPaths()
{
    const auto makePayload =
        [] ( const size_t size )
        {
            std::vector<uint8_t> result( size );
            for ( size_t i = 0; i < size; ++i ) {
                result[i] = static_cast<uint8_t>( ( i * 131U + 7U ) & 0xFFU );
            }
            return result;
        };

    const auto readBlock =
        [] ( gzip::BitReader& bitReader, deflate::Block<false>& block, const bool allowMarkers = false )
        {
            const auto headerError = block.readHeader( bitReader );
            REQUIRE( headerError == Error::NONE );
            std::vector<uint8_t> bytes;
            while ( !block.eob() ) {
                const auto [view, error] = block.read( bitReader, std::numeric_limits<size_t>::max() );
                REQUIRE( error == Error::NONE );
                for ( const auto& buffer : view.data ) {
                    bytes.insert( bytes.end(), buffer.begin(), buffer.end() );
                }
                for ( const auto& buffer : view.dataWithMarkers ) {
                    for ( const auto symbol : buffer ) {
                        if ( symbol > 255U ) {
                            REQUIRE( allowMarkers );
                            bytes.push_back( 0 );
                        } else {
                            bytes.push_back( static_cast<uint8_t>( symbol ) );
                        }
                    }
                }
            }
            return bytes;
        };

    /* Size >= MAX_WINDOW_SIZE reads the whole block at window position 0. */
    {
        const auto payload = makePayload( 65535 );
        DeflateBitWriter writer;
        writeStoredBlock( writer, payload, true );
        auto compressed = writer.finish();
        compressed.insert( compressed.end(), 8, 0 );

        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( compressed ) );
        deflate::Block<false> block;
        REQUIRE( readBlock( bitReader, block ) == payload );
    }

    /* A marker window whose marker-free suffix plus the stored block exceeds MAX_WINDOW_SIZE. */
    {
        constexpr size_t recentCount = 5000;
        constexpr size_t storedSize = 30000;
        constexpr size_t retainedCount = deflate::MAX_WINDOW_SIZE - storedSize;

        std::vector<uint8_t> recent( recentCount );
        for ( size_t i = 0; i < recent.size(); ++i ) {
            recent[i] = static_cast<uint8_t>( 'A' + ( i % 26 ) );
        }
        const auto payload = makePayload( storedSize );

        DeflateBitWriter writer;
        writer.writeBits( 0, 1 );  /* BFINAL */
        writer.writeBits( 1, 2 );  /* BTYPE = fixed Huffman */
        writeFixedLiteralOrLength( writer, 285 );  /* a 258 B match into the unknown window -> markers */
        writeFixedDistance( writer, 0, 0, 0 );     /* distance 1 */
        for ( const auto byte : recent ) {
            writeFixedLiteralOrLength( writer, byte );
        }
        writeFixedLiteralOrLength( writer, 256 );  /* end of block */
        writeStoredBlock( writer, payload, false );

        /* Reference the oldest retained byte of the recent suffix from the end of the stored block. */
        writer.writeBits( 1, 1 );  /* BFINAL */
        writer.writeBits( 1, 2 );  /* BTYPE = fixed Huffman */
        writeFixedLiteralOrLength( writer, 285 );       /* length 258 */
        writeFixedDistance( writer, 29, 8191, 13 );     /* distance 32768 */
        writeFixedLiteralOrLength( writer, 256 );       /* end of block */

        auto compressed = writer.finish();
        compressed.insert( compressed.end(), 8, 0 );

        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( compressed ) );
        deflate::Block<false> block;
        REQUIRE_EQUAL( readBlock( bitReader, block, true ).size(), 258U + recentCount );
        REQUIRE( readBlock( bitReader, block ) == payload );

        const std::vector<uint8_t> expected( recent.begin() + ( recentCount - retainedCount ),
                                             recent.begin() + ( recentCount - retainedCount + 258 ) );
        REQUIRE( readBlock( bitReader, block ) == expected );
    }

    /* A small stored block after a marker window with a short marker distance uses the fallback. */
    {
        const auto payload = makePayload( 30000 );
        DeflateBitWriter writer;
        writer.writeBits( 0, 1 );
        writer.writeBits( 1, 2 );
        writeFixedLiteralOrLength( writer, 285 );
        writeFixedDistance( writer, 0, 0, 0 );
        writeFixedLiteralOrLength( writer, 256 );
        writeStoredBlock( writer, payload, true );

        auto compressed = writer.finish();
        compressed.insert( compressed.end(), 8, 0 );

        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( compressed ) );
        deflate::Block<false> block;
        REQUIRE_EQUAL( readBlock( bitReader, block, true ).size(), 258U );
        REQUIRE( readBlock( bitReader, block ) == payload );
    }

    /* A real initial window is marker-free and takes the wrap-aware copy path. */
    {
        const auto payload = makePayload( 30000 );
        const auto initialWindow = makePayload( deflate::MAX_WINDOW_SIZE );
        DeflateBitWriter writer;
        writeStoredBlock( writer, payload, true );

        auto compressed = writer.finish();
        compressed.insert( compressed.end(), 8, 0 );

        gzip::BitReader bitReader( std::make_unique<BufferViewFileReader>( compressed ) );
        deflate::Block<false> block;
        block.setInitialWindow( { initialWindow.data(), initialWindow.size() } );
        REQUIRE( readBlock( bitReader, block ) == payload );
    }
}


int
main()
{
    testCalculateDistance();
    testCalculateLength();
    testFixedHuffmanCoding();
    testTruncatedStoredBlock();
    testAnalyzeTruncatedGzip();
    testInvalidFixedHuffmanLengthSymbols();
    testInvalidDynamicHuffmanCodeCount();
    testInvalidDeflateErrors();
    testMarkerBackreferenceCopies();
    testStoredBlockFastPaths();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
