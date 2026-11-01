# The harnesses use AFL++ persistent-mode macros that the compiler wrapper injects. Building them with a
# normal compiler produces undeclared __AFL_* identifiers, so fail early with a useful message.
if(NOT CMAKE_CXX_COMPILER MATCHES "afl")
    message(FATAL_ERROR
        "LIBRAPIDARCHIVE_BUILD_FUZZERS requires an AFL++ compiler wrapper. "
        "Configure a fresh build directory with CC=afl-clang-fast CXX=afl-clang-fast++.")
endif()

find_program(AFL_FUZZ_EXECUTABLE afl-fuzz)
if(NOT AFL_FUZZ_EXECUTABLE)
    message(FATAL_ERROR "afl-fuzz was not found. Install the afl++ package.")
endif()

set(AFL_FUZZ_SECONDS 60 CACHE STRING "Duration of each run-fuzz-* target in seconds; 0 runs until interrupted")

set(AFL_ASAN_OPTIONS "abort_on_error=1:halt_on_error=1:symbolize=0:detect_stack_use_after_return=1")
string(APPEND AFL_ASAN_OPTIONS ":max_malloc_fill_size=1073741824")

set(AFL_RUNTIME_ENV
    "AFL_SKIP_CPUFREQ=1"
    "AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1"
    "AFL_NO_UI=1"
    "AFL_AUTORESUME=1"
    "ASAN_OPTIONS=${AFL_ASAN_OPTIONS}"
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# Copy a small, target-specific corpus from the test data at configure time. AFL only needs the files
# once; the findings and the working corpus stay in the build directory.
function(addFuzzCorpus name)
    set(corpusDir "${CMAKE_BINARY_DIR}/fuzz-corpus/${name}")
    file(MAKE_DIRECTORY "${corpusDir}")
    foreach(pattern IN LISTS ARGN)
        file(GLOB files "${CMAKE_SOURCE_DIR}/src/tests/data/${pattern}")
        foreach(file IN LISTS files)
            file(SIZE "${file}" fileSize)
            if(fileSize LESS 10485760)
                file(COPY "${file}" DESTINATION "${corpusDir}")
            endif()
        endforeach()
    endforeach()
    set(FUZZ_CORPUS_${name} "${corpusDir}" PARENT_SCOPE)
endfunction()

function(addFuzzRunTarget name executable corpus)
    set(findingsDir "${CMAKE_BINARY_DIR}/fuzz-findings/${name}")
    file(MAKE_DIRECTORY "${findingsDir}")

    set(fuzzArgs -i "${corpus}" -o "${findingsDir}" -m none -t 3000+)
    if(AFL_FUZZ_SECONDS GREATER 0)
        list(APPEND fuzzArgs -V "${AFL_FUZZ_SECONDS}")
    endif()

    add_custom_target(run-fuzz-${name}
        COMMAND ${CMAKE_COMMAND} -E env ${AFL_RUNTIME_ENV}
                "${AFL_FUZZ_EXECUTABLE}" ${fuzzArgs} $<TARGET_FILE:${executable}>
        DEPENDS ${executable}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Fuzzing ${name} for ${AFL_FUZZ_SECONDS} seconds"
        VERBATIM)
endfunction()


add_executable(fuzzRapidgzip fuzzRapidgzip.cpp)
target_link_libraries(fuzzRapidgzip PRIVATE librapidgzip)

add_executable(fuzzDeflate fuzzDeflate.cpp)
target_link_libraries(fuzzDeflate PRIVATE librapidgzip)

add_executable(fuzzGzipIndex fuzzGzipIndex.cpp)
target_link_libraries(fuzzGzipIndex PRIVATE librapidgzip)

add_executable(fuzzIndexedBzip2 fuzzIndexedBzip2.cpp)
target_link_libraries(fuzzIndexedBzip2 PRIVATE librapidgzip)


addFuzzCorpus(rapidgzip
    "*.gz" "*.bgz" "*.zlib" "*.deflate" "*.igz" "*.migz" "*.pgzf" "*.pigz")
addFuzzCorpus(deflate "*.deflate")
addFuzzCorpus(gzip_index "*.index" "*.gztool.index" "*.gzi")
addFuzzCorpus(indexed_bzip2 "*.bz2")

addFuzzRunTarget(rapidgzip fuzzRapidgzip "${FUZZ_CORPUS_rapidgzip}")
addFuzzRunTarget(deflate fuzzDeflate "${FUZZ_CORPUS_deflate}")
addFuzzRunTarget(gzip-index fuzzGzipIndex "${FUZZ_CORPUS_gzip_index}")
addFuzzRunTarget(indexed-bzip2 fuzzIndexedBzip2 "${FUZZ_CORPUS_indexed_bzip2}")

add_custom_target(run-fuzz-all
    DEPENDS run-fuzz-rapidgzip run-fuzz-deflate run-fuzz-gzip-index run-fuzz-indexed-bzip2)
