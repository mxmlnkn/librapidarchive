#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <core/common.hpp>
#include <filereader/BufferView.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>

using namespace rapidgzip;

__AFL_FUZZ_INIT();


int
main()
{
    __AFL_INIT();

    const auto* const fuzzBuffer = __AFL_FUZZ_TESTCASE_BUF;
    while ( __AFL_LOOP( 10000 ) ) {
        const auto inputSize = static_cast<size_t>( __AFL_FUZZ_TESTCASE_LEN );
        if ( inputSize < 4 ) {
            continue;
        }

        /* Use a fresh, size-exact allocation for every iteration because ASan only reports reads past the
         * allocation, not past the vector size. A reused vector keeps the capacity of the largest previous
         * test case and would hide such over-reads. */
        const std::vector<char> input( fuzzBuffer, fuzzBuffer + inputSize );

        try {
            /* The sequential reader keeps AFL edge coverage deterministic; the parallel reader is covered by TSan. */
            indexed_bzip2::BZ2Reader reader(
                std::make_unique<rapidgzip::BufferViewFileReader>( input ) );
            std::array<char, 4_Ki> buffer{};
            size_t nBytesReadTotal{ 0 };
            while ( nBytesReadTotal < 16_Mi ) {
                const auto nBytesRead = reader.read( buffer.data(), buffer.size() );
                if ( nBytesRead == 0 ) {
                    break;
                }
                nBytesReadTotal += nBytesRead;
            }
        } catch ( const std::bad_alloc& ) {
            std::abort();
        } catch ( const std::length_error& ) {
            std::abort();
        } catch ( ... ) {
            /* Malformed input is expected to throw descriptive exceptions. */
        }
    }

    return 0;
}
