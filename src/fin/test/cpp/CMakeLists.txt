# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors

include("openroad")

# The metal density rule engine (fin/density_check.h) is header-only and
# dependency-free, so the correctness gate links no OpenDB/boost machinery.
add_executable(TestDensityCheck TestDensityCheck.cpp)

target_include_directories(TestDensityCheck
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(TestDensityCheck
  GTest::gtest
  GTest::gtest_main
)

gtest_discover_tests(TestDensityCheck
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(build_and_test
  TestDensityCheck
)
