FetchContent_Declare(
  extern_catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  )
if(NOT extern_catch2_POPULATED)
  FetchContent_Populate(extern_catch2)
  add_subdirectory(
    ${extern_catch2_SOURCE_DIR}
    ${extern_catch2_BINARY_DIR}
    EXCLUDE_FROM_ALL)

endif()
list(APPEND CMAKE_MODULE_PATH "${extern_catch2_SOURCE_DIR}/contrib/")
include(CTest)
include(Catch)
enable_testing()
set(UNIT_TEST_LIST
  test_bgen_header_format
  test_little_endian
  test_variant_data_block
  test_bgen_snp_format
  test_utils)




add_executable(tests unit/main.cpp unit/test_bgen_header_format.cpp unit/test_little_endian.cpp unit/test_variant_data_block.cpp
  unit/ProbSetCheck.cpp unit/test_bgen_snp_format.cpp unit/test_utils.cpp unit/ProbSetCheck.hpp unit/test_case.hpp unit/test_utils.hpp)
target_link_libraries(tests bgen)
target_link_libraries(tests Catch2::Catch2)
include(ParseAndAddCatchTests)

catch_discover_tests(tests)
