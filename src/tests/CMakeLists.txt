add_library(all_tests INTERFACE)

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
