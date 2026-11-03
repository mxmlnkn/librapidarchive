function(addIndexedBzip2Test name)
    add_executable(${name})
    target_sources(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/${name}.cpp)
    target_link_libraries(${name} PRIVATE indexed_bzip2_parallel)
    add_test(NAME ${name} COMMAND ${name})
    if(SANITIZER_TEST_ENVIRONMENT)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${SANITIZER_TEST_ENVIRONMENT}")
    endif()
    add_dependencies(all_tests ${name})
endfunction()


# Calling bzip2.exe via std::system does not work for some reason on the Windows Github CI runners.
# It quits with: bzip2: Can't open input file 'decoded': Invalid argument.
if(NOT WIN32)
addIndexedBzip2Test(testBZ2Reader)
addIndexedBzip2Test(testParallelBZ2Reader)
endif()

add_executable(testRunAB)
target_sources(testRunAB PRIVATE ${CMAKE_CURRENT_LIST_DIR}/testRunAB.cpp)
target_link_libraries(testRunAB PRIVATE core)
add_test(NAME Test-Run-AB COMMAND testRunAB)
# Temporary for the Windows hang diagnosis.
set_tests_properties(Test-Run-AB PROPERTIES TIMEOUT 600)
add_dependencies(all_tests testRunAB)
if(SANITIZER_TEST_ENVIRONMENT)
    set_tests_properties(Test-Run-AB PROPERTIES ENVIRONMENT "${SANITIZER_TEST_ENVIRONMENT}")
endif()
