# OVERSHELL: interactive shell frontend (input, parsing, dispatch, UX).
# Links the KNRL engine library; owns CLI scenarios and tests.
add_executable(override
    src/main.cpp
    src/shell.cpp
    src/shell_help.cpp
    src/parser.cpp
    src/scenario.cpp
)
target_include_directories(override PRIVATE include)
target_link_libraries(override PRIVATE knrl)
if(MSVC)
    target_compile_options(override PRIVATE /W4 /permissive-)
else()
    target_compile_options(override PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Engine-level test suite (uses Shell only for non-crashing malformed-input
# checks, so it compiles the shell frontend TU set too -- the knrl library
# itself stays engine-only).
add_executable(mvp_test
    tests/mvp_test.cpp
    src/shell.cpp
    src/shell_help.cpp
    src/parser.cpp
    src/scenario.cpp
)
target_include_directories(mvp_test PRIVATE include)
target_link_libraries(mvp_test PRIVATE knrl)
if(NOT MSVC)
    target_compile_options(mvp_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(override mvp_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_test(NAME mvp_flow COMMAND mvp_test)
