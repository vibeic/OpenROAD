load("@rules_cc//cc:cc_test.bzl", "cc_test")

# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025-2026, The OpenROAD Authors

package(features = ["layering_check"])

# From CMakeLists.txt add_executable(TestDensityCheck) + gtest_discover_tests.
# etc/Build.sh defaults to Bazel, so the CMake registration alone never runs in
# the build that produces the shipped binary.
#
# TestDensityCheck.cpp includes only <string>, <vector>, "fin/density_check.h"
# and gtest, so it links //src/fin:density_check_hdr rather than //src/fin --
# the same "no OpenDB/boost machinery" property the CMake target has.
cc_test(
    name = "density_check_unittest",
    srcs = ["TestDensityCheck.cpp"],
    deps = [
        "//src/fin:density_check_hdr",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
