# ORDC: deterministic .ord document model (parse/serialize/diagnostics).
# Pure data handling: no simulation, no filesystem, no host interaction.
add_library(ordc STATIC
    src/ord.cpp
)
target_include_directories(ordc PUBLIC include)
if(MSVC)
    target_compile_options(ordc PRIVATE /W4 /permissive-)
else()
    target_compile_options(ordc PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ordc inspection tool: check/show .ord files (reads only, never writes).
add_executable(ordc-tool tool/ordc.cpp)
target_link_libraries(ordc-tool PRIVATE ordc)
set_target_properties(ordc-tool PROPERTIES
    OUTPUT_NAME ordc
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Fixture files for tool-level tests (written at configure time; the tool
# only reads them).
file(WRITE "${CMAKE_BINARY_DIR}/ordc-fixtures/good.ord"
    "ORD 1\ntype = world\nid = w1\n\n[meta]\nname = Fixture\n")
file(WRITE "${CMAKE_BINARY_DIR}/ordc-fixtures/bad-version.ord" "ORD 2\ntype = world\nid = w1\n")
file(WRITE "${CMAKE_BINARY_DIR}/ordc-fixtures/bad-syntax.ord"
    "ORD 1\ntype = world\nid = w1\n[meta]\nname = 1\nname = 2\n")

add_test(NAME ordc_good
    COMMAND ordc-tool check "${CMAKE_BINARY_DIR}/ordc-fixtures/good.ord")
add_test(NAME ordc_bad_version
    COMMAND ordc-tool check "${CMAKE_BINARY_DIR}/ordc-fixtures/bad-version.ord")
add_test(NAME ordc_bad_syntax
    COMMAND ordc-tool check "${CMAKE_BINARY_DIR}/ordc-fixtures/bad-syntax.ord")
# Rejection IS the expected behavior for malformed inputs.
set_tests_properties(ordc_bad_version ordc_bad_syntax PROPERTIES WILL_FAIL TRUE)

