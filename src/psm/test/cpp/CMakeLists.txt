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

# The EM current-density signoff rule engine (em_signoff.h) is likewise
# header-only and dependency-free, so the correctness gate links no
# OpenDB/OpenSTA machinery.
add_executable(TestEMSignoff TestEMSignoff.cpp)

target_include_directories(TestEMSignoff
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(TestEMSignoff
  GTest::gtest
  GTest::gtest_main
)

gtest_discover_tests(TestEMSignoff
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# The analysis-driven PDN sizing engine (pdn_sizing.h) is header-only; the gate
# additionally builds an independent Eigen ladder-network solve to check the
# closed-form rail droop against, so it links Eigen but no OpenDB/OpenSTA.
add_executable(TestPdnSizing TestPdnSizing.cpp)

target_include_directories(TestPdnSizing
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(TestPdnSizing
  GTest::gtest
  GTest::gtest_main
  Eigen3::Eigen
)

gtest_discover_tests(TestPdnSizing
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# The droop-driven decap sizer (decap_opt.h) is cross-checked against the
# transient MNA kernel, so this gate needs both include roots plus Eigen.
add_executable(TestDecapOpt TestDecapOpt.cpp)

target_include_directories(TestDecapOpt
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(TestDecapOpt
  GTest::gtest
  GTest::gtest_main
  Eigen3::Eigen
)

gtest_discover_tests(TestDecapOpt
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(build_and_test
  TestTransientRC
  TestEMSignoff
  TestPdnSizing
  TestDecapOpt
)
