#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <filereader/BufferView.hpp>
#include <rapidgzip/IndexFileFormat.hpp>

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
            (void)rapidgzip::readGzipIndex(
                std::make_unique<rapidgzip::BufferViewFileReader>( input ) );
        } catch ( const std::bad_alloc& ) {
            std::abort();
        } catch ( const std::length_error& ) {
            std::abort();
        } catch ( ... ) {
            /* Unknown index variants must fail with descriptive exceptions. */
        }
    }

    return 0;
}
