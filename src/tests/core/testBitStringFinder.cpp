#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

#include <core/BitStringFinder.hpp>
#include <core/common.hpp>
#include <core/TestHelpers.hpp>
#include <filereader/Standard.hpp>


using namespace rapidgzip;


namespace
{
constexpr uint64_t BZIP2_MAGIC = 0x314159265359ULL;


[[nodiscard]] std::vector<uint8_t>
makeRandomBuffer( std::mt19937& random,
                  const size_t  size )
{
    std::vector<uint8_t> buffer( size );
    for ( auto& byte : buffer ) {
        byte = static_cast<uint8_t>( random() );
    }
    return buffer;
}


/** Write the low @p bitStringSize bits of @p bitString into @p buffer MSB-first at bit position @p bitPosition. */
void
writeBits( std::vector<uint8_t>& buffer,
           const uint64_t        bitString,
           const uint8_t         bitStringSize,
           const size_t          bitPosition )
{
    for ( uint8_t i = 0; i < bitStringSize; ++i ) {
        const auto bit = static_cast<uint8_t>( ( bitString >> ( bitStringSize - 1U - i ) ) & 1U );
        const auto absoluteBit = bitPosition + i;
        const auto byteIndex = absoluteBit / CHAR_BIT;
        const auto bitInByte = static_cast<uint8_t>( CHAR_BIT - 1U - ( absoluteBit % CHAR_BIT ) );
        buffer[byteIndex] = static_cast<uint8_t>( ( buffer[byteIndex] & ~( 1U << bitInByte ) ) | ( bit << bitInByte ) );
    }
}


template<uint8_t bitStringSize>
bool
testFastFinderMatchesGenericView( const std::string_view view,
                                  const uint64_t         bitString )
{
    const auto expected = detail::findBitStringsGeneric<bitStringSize>( view, bitString );

    bool success = true;
    for ( const auto kernel : { detail::BitStringFinderKernel::PORTABLE,
                                detail::BitStringFinderKernel::SSE2,
                                detail::BitStringFinderKernel::AVX2,
                                detail::BitStringFinderKernel::AVX512 } ) {
        if ( !rapidgzip::simd::bitStringFinder::isSupported( kernel ) ) {
            continue;
        }

        const auto actual = detail::findBitStringsFast<bitStringSize>( view, bitString, kernel );
        ++gnTests;
        if ( actual != expected ) {
            ++gnTestErrors;
            success = false;
            std::cerr << "[FAIL] Fast " << rapidgzip::simd::toString( kernel ) << " finder for " << view.size()
                      << " B and 0x" << std::hex << bitString << std::dec
                      << " returned " << actual << " instead of " << expected << "\n";
        }
    }

    const auto automatic = BitStringFinder<bitStringSize>::findBitStrings( view, bitString );
    ++gnTests;
    if ( automatic != expected ) {
        ++gnTestErrors;
        success = false;
        std::cerr << "[FAIL] Automatic finder returned " << automatic << " instead of " << expected << "\n";
    }

    return success;
}


template<uint8_t bitStringSize>
bool
testFastFinderMatchesGeneric( const std::vector<uint8_t>& buffer,
                              const uint64_t              bitString )
{
    return testFastFinderMatchesGenericView<bitStringSize>(
        { reinterpret_cast<const char*>( buffer.data() ), buffer.size() }, bitString );
}


template<uint8_t bitStringSize>
void
testFastFinderDifferential()
{
    std::mt19937 random( 0x3141'5926U + bitStringSize );

    /* Every supported buffer length around the SIMD vector widths and the needle length. */
    for ( const auto size : { 0, 1, 2, 3, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 66, 80,
                              127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025 } ) {
        for ( int iteration = 0; iteration < 20; ++iteration ) {
            auto buffer = makeRandomBuffer( random, size );
            const auto bitString = static_cast<uint64_t>( random() ) & nLowestBitsSet<uint64_t>( bitStringSize );
            testFastFinderMatchesGeneric<bitStringSize>( buffer, bitString );
        }
    }

    /* Embed a random pattern at every bit position of a small buffer. */
    for ( const auto size : { 9, 10, 16, 17, 33 } ) {
        const auto bufferBits = static_cast<size_t>( size ) * CHAR_BIT;
        if ( bufferBits < bitStringSize ) {
            continue;
        }
        for ( size_t bitPosition = 0; bitPosition + bitStringSize <= bufferBits; ++bitPosition ) {
            auto buffer = makeRandomBuffer( random, size );
            const auto bitString = static_cast<uint64_t>( random() ) & nLowestBitsSet<uint64_t>( bitStringSize );
            writeBits( buffer, bitString, bitStringSize, bitPosition );
            testFastFinderMatchesGeneric<bitStringSize>( buffer, bitString );
        }
    }

    /* Positions around the 16/32/64-byte vector widths and the buffer end. */
    for ( const auto size : { 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025 } ) {
        const auto bufferBits = static_cast<size_t>( size ) * CHAR_BIT;
        for ( const auto position : { size_t{ 0 }, size_t{ 1 }, size_t{ 7 }, size_t{ 8 }, size_t{ 15 },
                                      size_t{ 16 }, size_t{ 31 }, size_t{ 32 }, size_t{ 63 }, size_t{ 64 },
                                      size_t{ 127 }, size_t{ 128 }, size_t{ 255 }, size_t{ 256 },
                                      size_t{ 511 }, size_t{ 512 }, size_t{ 1023 }, bufferBits - bitStringSize } ) {
            if ( position + bitStringSize > bufferBits ) {
                continue;
            }
            auto buffer = makeRandomBuffer( random, size );
            const auto bitString = static_cast<uint64_t>( random() ) & nLowestBitsSet<uint64_t>( bitStringSize );
            writeBits( buffer, bitString, bitStringSize, position );
            testFastFinderMatchesGeneric<bitStringSize>( buffer, bitString );
        }
    }

    /* Unaligned view starts exercise the unaligned SIMD loads. */
    for ( const auto offset : { size_t{ 0 }, size_t{ 1 }, size_t{ 3 }, size_t{ 7 }, size_t{ 8 }, size_t{ 15 },
                                size_t{ 16 }, size_t{ 17 }, size_t{ 31 }, size_t{ 32 }, size_t{ 33 }, size_t{ 63 } } ) {
        for ( const auto size : { size_t{ 64 }, size_t{ 65 }, size_t{ 127 }, size_t{ 128 }, size_t{ 129 },
                                  size_t{ 255 }, size_t{ 256 }, size_t{ 257 }, size_t{ 511 }, size_t{ 512 },
                                  size_t{ 513 } } ) {
            auto storage = makeRandomBuffer( random, size + offset + 8 );
            const auto bufferBits = size * CHAR_BIT;
            for ( const auto position : { size_t{ 0 }, bufferBits - bitStringSize } ) {
                if ( position + bitStringSize > bufferBits ) {
                    continue;
                }
                const auto bitString = static_cast<uint64_t>( random() ) & nLowestBitsSet<uint64_t>( bitStringSize );
                writeBits( storage, bitString, bitStringSize, offset * CHAR_BIT + position );
                testFastFinderMatchesGenericView<bitStringSize>(
                    { reinterpret_cast<const char*>( storage.data() + offset ), size }, bitString );
            }
        }
    }

    /* Overlapping occurrences at byte boundaries must all be found. */
    for ( const auto size : { 64, 128, 256, 1024 } ) {
        auto buffer = makeRandomBuffer( random, size );
        const auto bitString = static_cast<uint64_t>( random() ) & nLowestBitsSet<uint64_t>( bitStringSize );
        for ( size_t position = 0; position + bitStringSize <= static_cast<size_t>( size ) * CHAR_BIT;
              position += CHAR_BIT ) {
            writeBits( buffer, bitString, bitStringSize, position );
        }
        testFastFinderMatchesGeneric<bitStringSize>( buffer, bitString );
    }

    /* The bzip2 magic at all eight bit shifts, including the tail positions at the buffer end. */
    const auto maskedMagic = BZIP2_MAGIC & nLowestBitsSet<uint64_t>( bitStringSize );
    for ( uint8_t shift = 0; shift < CHAR_BIT; ++shift ) {
        for ( const auto size : { 8, 9, 10, 16, 17, 32, 33 } ) {
            if ( shift + bitStringSize > size * CHAR_BIT ) {
                continue;
            }
            auto buffer = makeRandomBuffer( random, size );
            writeBits( buffer, maskedMagic, bitStringSize, shift );
            const auto expected = detail::findBitStringsGeneric<bitStringSize>(
                { reinterpret_cast<const char*>( buffer.data() ), buffer.size() }, maskedMagic );
            ++gnTests;
            if ( std::find( expected.begin(), expected.end(), shift ) == expected.end() ) {
                ++gnTestErrors;
                std::cerr << "[FAIL] Reference finder missed the magic at bit shift " << int( shift ) << "\n";
            }
            testFastFinderMatchesGeneric<bitStringSize>( buffer, maskedMagic );
        }
    }
}
}  // namespace


template<class TemplatedBitStringFinder>
bool
testBitStringFinder( TemplatedBitStringFinder&& bitStringFinder,
                     const std::vector<size_t>& stringPositions )
{
    /* Gather all strings (time out at 1k strings because tests are written manually
     * and never will require that many matches, so somethings must have gone wrong */
    std::vector<size_t> matches;
    for ( int i = 0; i < 16; ++i ) {
        const auto match = bitStringFinder.find();
        if ( !match ) {
            break;
        }
        matches.push_back( static_cast<size_t>( *match ) );
    }

    ++gnTests;
    if ( matches != stringPositions ) {
        ++gnTestErrors;
        std::cerr << "[FAIL] Matches: " << matches << " != " << stringPositions << "\n";
        return false;
    }

    return true;
}


template<uint8_t bitStringSize>
void
testBitStringFinder( uint64_t                          bitStringToFind,
                     const std::vector<unsigned char>& buffer,
                     const std::vector<size_t>&        stringPositions )
{
    auto const * const rawBuffer = reinterpret_cast<const char*>( buffer.data() );

    {
        /* test the version working on an input buffer */
        BitStringFinder<bitStringSize> bitStringFinder( rawBuffer, buffer.size(), bitStringToFind );
        if ( !testBitStringFinder( std::move( bitStringFinder ), stringPositions ) ) {
            std::cerr << "Version working on input buffer failed!\n";
        }
    }

    {
        /* test the version working on an input file by writing the buffer to a temporary file */
        auto const file = make_unique_file_ptr( std::tmpfile() );  // NOLINT
        std::fwrite( buffer.data(), sizeof( buffer[0] ), buffer.size(), file.get() );
        /**
         * Flush the file so that BitReader sees the written data when accessing the file through the file descriptor.
         * Don't close file because:
         * > On some implementations (e.g. Linux), this function actually creates, opens, and immediately deletes
         * > the file from the file system
         * @see https://en.cppreference.com/w/cpp/io/c/tmpfile
         * Also, use smallest sane value for fileBufferSizeBytes = sizeof( uint64_t ) in order to check that
         * recognizing bit strings across file buffer borders works correctly.
         */
        std::fflush( file.get() );
        BitStringFinder<bitStringSize> bitStringFinder(
            std::make_unique<StandardFileReader>( fileno( file.get() ) ),
            bitStringToFind,
            sizeof( uint64_t )
        );
        if ( !testBitStringFinder( std::move( bitStringFinder ), stringPositions ) ) {
            std::cerr << "Version working on input file failed!\n";
        }
    }
}


int
main()
{
    /* 0-size bit strings to find arguably makes no sense to test for. */
    //testBitStringFinder<0>( 0b0, {}, {} );
    //testBitStringFinder<0>( 0b0, { 0x00 }, {} );
    //testBitStringFinder<0>( 0b1111'1111, {}, {} );
    //testBitStringFinder<0>( 0b1111'1111, { 0x00 }, {} );

    /* BitStringReader is optimized and only works with bit strings that are multiplies of and greater than 8. */
    /*
    testBitStringFinder<1>( 0b0, { 0b0000'1111 }, { 0, 1, 2, 3 } );
    testBitStringFinder<1>( 0b0, { 0b1010'1010 }, { 1, 3, 5, 7 } );
    testBitStringFinder<1>( 0b0, { 0b1111'1111 }, {} );
    testBitStringFinder<1>( 0b0, { 0b0111'1111, 0b1111'1110 }, { 0, 15 } );
    testBitStringFinder<2>( 0b0, { 0b0000'1111 }, { 0, 1, 2 } );
    testBitStringFinder<3>( 0b0, { 0b0000'1111 }, { 0, 1 } );
    testBitStringFinder<4>( 0b0, { 0b0000'1111 }, { 0 } );
    testBitStringFinder<5>( 0b0, { 0b0000'1111 }, {} );

    testBitStringFinder<1>( 0b1111'1111, { 0b0000'1111 }, { 4, 5, 6, 7 } );
    testBitStringFinder<1>( 0b1111'1111, { 0b1010'1010 }, { 0, 2, 4, 6 } );
    testBitStringFinder<8>( 0b1111'1111, { 0b1111'1111 }, { 0 } );
    testBitStringFinder<1>( 0b1111'1111, { 0b1000'0000, 0b0000'0001 }, { 0, 15 } );
    testBitStringFinder<2>( 0b1111'1111, { 0b0000'1111 }, { 4, 5, 6 } );
    testBitStringFinder<3>( 0b1111'1111, { 0b0000'1111 }, { 4, 5 } );
    testBitStringFinder<4>( 0b1111'1111, { 0b0000'1111 }, { 4 } );
    testBitStringFinder<5>( 0b1111'1111, { 0b0000'1111 }, {} );

    testBitStringFinder<10>( 0b10'1010'1010, { 0b0101'0101, 0b0101'0101 }, { 1, 3, 5 } );
    */
    testBitStringFinder<48>( 0x314159265359ULL, { 0x11, 0x41, 0x59, 0x26, 0x53, 0x59 }, {} );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x58 }, {} );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59 }, { 0 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 0 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 8 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 16 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 24 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0, 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 }, { 32 } );

    testBitStringFinder<48>( 0x314159265359ULL, { 0x18, 0xA0, 0xAC, 0x93, 0x29, 0xAC, 0x80 }, { 1 } );
    testBitStringFinder<48>( 0x314159265359ULL, { 0x00, 0x62, 0x82, 0xB2, 0x4C, 0xA6, 0xB2 }, { 7 } );

    /* Tests with second match a lot further away and definitely over the loading chunk size. */
    {
        const std::vector<unsigned char> buffer = { 0, 0, 0, 0, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0, 0 };
        const std::vector<size_t> expectedResults = { 32 };
        for ( const auto offset : { 1, 100, 123, 1024, 2000 } ) {
            auto tmpResults = expectedResults;
            tmpResults.push_back( ( buffer.size() + offset ) * CHAR_BIT );

            auto tmpBuf = buffer;
            tmpBuf.resize( tmpBuf.size() + offset, 0 );
            for ( auto c : { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59 } ) {
                tmpBuf.push_back( c );
            }

            testBitStringFinder<48>( 0x314159265359ULL, tmpBuf, tmpResults );
        }
    }

    testFastFinderDifferential<16>();
    testFastFinderDifferential<24>();
    testFastFinderDifferential<32>();
    testFastFinderDifferential<40>();
    testFastFinderDifferential<48>();
    testFastFinderDifferential<56>();

    std::cout << "Tests successful: " << ( gnTests - gnTestErrors ) << " / " << gnTests << "\n";

    return gnTestErrors == 0 ? 0 : 1;
}
