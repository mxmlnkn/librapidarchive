add_library(all_tests INTERFACE)

# CTest-only runtime options. Set the sanitizer environment on every test so `cmake --build -- check`
# and direct ctest invocations get the same abort/print behavior without shell wrappers.
set(SANITIZER_TEST_ENVIRONMENT "")
if(CMAKE_BUILD_TYPE STREQUAL "Sanitize")
    set(SANITIZER_ASAN_OPTIONS "abort_on_error=1:halt_on_error=1:detect_leaks=0:detect_stack_use_after_return=1")
    string(APPEND SANITIZER_ASAN_OPTIONS ":max_malloc_fill_size=1073741824")
    set(SANITIZER_TEST_ENVIRONMENT
        "ASAN_OPTIONS=${SANITIZER_ASAN_OPTIONS}"
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
elseif(CMAKE_BUILD_TYPE STREQUAL "ThreadSanitize")
    set(SANITIZER_TEST_ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
endif()

# Enable standard-library bounds and hardening checks for all tests. These are test-only definitions;
# they do not affect the installed library or its ABI.
if ( CMAKE_CXX_COMPILER_ID STREQUAL "GNU" )
    add_compile_definitions(_GLIBCXX_ASSERTIONS)
elseif ( CMAKE_CXX_COMPILER_ID MATCHES "Clang" )
    add_compile_definitions(_GLIBCXX_ASSERTIONS)
    add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
endif()

add_subdirectory(core)
add_subdirectory(indexed_bzip2)
add_subdirectory(rapidgzip)

if (LIBRAPIDARCHIVE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
