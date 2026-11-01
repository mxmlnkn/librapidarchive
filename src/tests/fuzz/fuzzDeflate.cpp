#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <core/common.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/definitions.hpp>
#include <rapidgzip/gzip/deflate.hpp>

using namespace rapidgzip;

__AFL_FUZZ_INIT();


int
main()
{
    __AFL_INIT();

    const auto* const fuzzBuffer = __AFL_FUZZ_TESTCASE_BUF;
    while ( __AFL_LOOP( 10000 ) ) {
        const auto inputSize = static_cast<size_t>( __AFL_FUZZ_TESTCASE_LEN );
        if ( inputSize < 1 ) {
            continue;
        }

        /* Use a fresh, size-exact allocation for every iteration because ASan only reports reads past the
         * allocation, not past the vector size. A reused vector keeps the capacity of the largest previous
         * test case and would hide such over-reads. */
        const std::vector<char> input( fuzzBuffer, fuzzBuffer + inputSize );

        try {
            gzip::BitReader reader( std::make_unique<BufferViewFileReader>( input ) );
            deflate::Block<> block;
            const auto headerError = block.readHeader( reader );
            if ( headerError != Error::NONE ) {
                continue;
            }

            while ( !block.eob() ) {
                const auto [data, error] = block.read( reader, 1_Ki );
                if ( ( error != Error::NONE ) || ( data.size() == 0 ) ) {
                    break;
                }
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
