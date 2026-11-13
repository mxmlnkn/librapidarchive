#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <core/TestHelpers.hpp>
#include <filereader/BufferView.hpp>
#include <indexed_bzip2/BZ2Reader.hpp>
#include <indexed_bzip2/ParallelBZ2Reader.hpp>


using namespace indexed_bzip2;
using namespace rapidgzip;


namespace
{
/** BZh9, block magic, zero block CRC, isRandomized=0, origPtr=0, and 16 zero symbol-map bits. */
const std::vector<uint8_t> EMPTY_SYMBOL_MAP_STREAM = {
    'B', 'Z', 'h', '9', 0x31, 0x41, 0x59, 0x26, 0x53, 0x59,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};


template<typename Factory>
void
checkEmptySymbolMap( Factory makeReader )
{
    auto reader = makeReader( EMPTY_SYMBOL_MAP_STREAM );

    std::vector<char> buffer( 4_Ki );
    std::string message;
    try {
        while ( reader->read( buffer.data(), buffer.size() ) > 0 ) {}
    } catch ( const std::exception& exception ) {
        message = exception.what();
    }
    REQUIRE( message.find( "symbol map is empty" ) != std::string::npos );
}
}  // namespace


int
main()
{
    checkEmptySymbolMap(
        [] ( const std::vector<uint8_t>& data )
        {
            return std::make_unique<BZ2Reader>( std::make_unique<BufferViewFileReader>( data ) );
        } );

    for ( const auto parallelization : { 1U, 2U } ) {
        checkEmptySymbolMap(
            [parallelization] ( const std::vector<uint8_t>& data )
            {
                return std::make_unique<ParallelBZ2Reader>(
                    std::make_unique<BufferViewFileReader>( data ), parallelization );
            } );
    }
}
