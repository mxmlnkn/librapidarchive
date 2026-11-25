# Keep this in the subdirectory so that consumers adding only src/ get the same default. Defining the
# macro with an empty value would bypass the fallback in Bzip2Crc32.hpp and break the build.
set(LIBRAPIDARCHIVE_BZIP2_CRC_SLICES "8" CACHE STRING
    "Number of bytes per bzip2 CRC slicing iteration. Must be 1, 4, or 8.")
if(NOT LIBRAPIDARCHIVE_BZIP2_CRC_SLICES MATCHES "^(1|4|8)$")
    message(FATAL_ERROR "LIBRAPIDARCHIVE_BZIP2_CRC_SLICES must be 1, 4, or 8, not "
                        "'${LIBRAPIDARCHIVE_BZIP2_CRC_SLICES}'.")
endif()

add_library(indexed_bzip2 INTERFACE)
target_include_directories(indexed_bzip2 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/..)
target_sources(indexed_bzip2 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/BZ2Reader.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2Compressor.hpp
    ${CMAKE_CURRENT_LIST_DIR}/BZ2ReaderInterface.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2Format.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2InverseBWT.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2MoveToFront.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2SymbolDecoder.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2SymbolKernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/IndexFileFormat.hpp
    ${CMAKE_CURRENT_LIST_DIR}/bzip2.hpp
)
target_link_libraries(indexed_bzip2 INTERFACE core libhuffman libsais)
target_compile_features(indexed_bzip2 INTERFACE cxx_std_17)
target_compile_definitions(indexed_bzip2 INTERFACE
    LIBRAPIDARCHIVE_BZIP2_CRC_SLICES=${LIBRAPIDARCHIVE_BZIP2_CRC_SLICES})


add_library(indexed_bzip2_parallel INTERFACE)
target_include_directories(indexed_bzip2_parallel INTERFACE ${CMAKE_CURRENT_LIST_DIR}/..)
target_sources(indexed_bzip2_parallel INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/BZ2BlockFetcher.hpp
    ${CMAKE_CURRENT_LIST_DIR}/BZ2Reader.hpp
    ${CMAKE_CURRENT_LIST_DIR}/BZ2ReaderInterface.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2Format.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2InverseBWT.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2MoveToFront.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2SymbolDecoder.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Bzip2SymbolKernel.hpp
    ${CMAKE_CURRENT_LIST_DIR}/IndexFileFormat.hpp
    ${CMAKE_CURRENT_LIST_DIR}/bzip2.hpp
    ${CMAKE_CURRENT_LIST_DIR}/ParallelBZ2Reader.hpp
)
target_link_libraries(indexed_bzip2_parallel INTERFACE core libhuffman libsais Threads::Threads)
target_compile_features(indexed_bzip2_parallel INTERFACE cxx_std_17)
target_compile_definitions(indexed_bzip2_parallel INTERFACE
    LIBRAPIDARCHIVE_BZIP2_CRC_SLICES=${LIBRAPIDARCHIVE_BZIP2_CRC_SLICES})
