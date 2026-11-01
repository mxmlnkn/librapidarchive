#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <core/common.hpp>
#include <filereader/BufferView.hpp>
#include <rapidgzip/gzip/GzipReader.hpp>
#include <rapidgzip/ParallelGzipReader.hpp>

using namespace rapidgzip;

__AFL_FUZZ_INIT();

namespace
{
constexpr size_t MAX_DECOMPRESSED_SIZE = 16_Mi;
constexpr size_t OUTPUT_BUFFER_SIZE = 4_Ki;


void
readAll( rapidgzip::FileReader& reader )
{
    std::array<char, OUTPUT_BUFFER_SIZE> buffer{};
    size_t nBytesReadTotal{ 0 };
    while ( nBytesReadTotal < MAX_DECOMPRESSED_SIZE ) {
        const auto nBytesRead = reader.read( buffer.data(), buffer.size() );
        if ( nBytesRead == 0 ) {
            break;
        }
        nBytesReadTotal += nBytesRead;
    }
}


void
runSequential( const std::vector<char>& input )
{
    rapidgzip::GzipReader reader( std::make_unique<rapidgzip::BufferViewFileReader>( input ) );
    reader.setCRC32Enabled( true );
    readAll( reader );
}


void
runParallel( const std::vector<char>& input,
             const size_t             parallelization,
             const size_t             chunkSize )
{
    rapidgzip::ParallelGzipReader<rapidgzip::ChunkData> reader(
        std::make_unique<rapidgzip::BufferViewFileReader>( input ), parallelization, chunkSize );
    reader.setCRC32Enabled( true );
    readAll( reader );
}
}  // namespace


int
main()
{
    __AFL_INIT();

    const auto* const fuzzBuffer = __AFL_FUZZ_TESTCASE_BUF;
    while ( __AFL_LOOP( 10000 ) ) {
        const auto inputSize = static_cast<size_t>( __AFL_FUZZ_TESTCASE_LEN );
        if ( inputSize < 2 ) {
            continue;
        }

        /* AFL's buffer is larger than the reported size. Copy into a fresh, size-exact allocation for every
         * iteration because ASan only reports reads past the allocation, not past the vector size. Resizing a
         * reused vector keeps the capacity of the largest previous test case and would hide such over-reads. */
        const std::vector<char> input( fuzzBuffer, fuzzBuffer + inputSize );

        try {
            const auto mode = inputSize % 3;
            if ( mode == 0 ) {
                runSequential( input );
            } else {
                runParallel( input, mode, 1_Ki << ( inputSize % 4 ) );
            }
        } catch ( const std::bad_alloc& ) {
            /* An untrusted size must not drive an allocation that can exhaust memory. */
            std::abort();
        } catch ( const std::length_error& ) {
            std::abort();
        } catch ( ... ) {
            /* Malformed input is expected to throw descriptive exceptions. */
        }
    }

    return 0;
}
