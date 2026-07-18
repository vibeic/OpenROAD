# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors

include("openroad")

find_package(Eigen3 REQUIRED)

# The transient MNA kernel (transient.h) is header-only and depends solely on
# Eigen, so the analytic-RC correctness gate links no OpenDB/OpenSTA machinery.
add_executable(TestTransientRC TestTransientRC.cpp)

target_include_directories(TestTransientRC
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(TestTransientRC
  GTest::gtest
  GTest::gtest_main
  Eigen3::Eigen
)

gtest_discover_tests(TestTransientRC
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(build_and_test
  TestTransientRC
)
