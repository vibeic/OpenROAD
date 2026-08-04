# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025-2026, The OpenROAD Authors

# vibeic fork file.  These five gtest binaries existed ONLY in
# src/psm/test/cpp/CMakeLists.txt.  etc/Build.sh has defaulted to bazel since
# the 2026-07-29 daily merge, so until this file existed none of them ran in the
# pipeline that produces the shipped binary -- the psm transient, EM-signoff,
# signal-EM, PDN-sizing and decap engines shipped with their unit coverage
# switched off, while `FIX_STATUS.md` recorded a FAIL->PASS proof for each.
#
# Test Parity Note: every add_executable in ../cpp/CMakeLists.txt has a cc_test
# here.  Keep the two in step; etc/check_test_registration_parity.py enforces it.

load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(features = ["layering_check"])

# The transient MNA kernel (transient.h) is header-only and depends solely on
# Eigen, so the analytic-RC correctness gate links no OpenDB/OpenSTA machinery.
cc_test(
    name = "TestTransientRC",
    srcs = ["TestTransientRC.cpp"],
    deps = [
        "//src/psm:analysis_headers",
        "@eigen",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# The EM current-density signoff rule engine (em_signoff.h) is likewise
# header-only and dependency-free.
cc_test(
    name = "TestEMSignoff",
    srcs = ["TestEMSignoff.cpp"],
    deps = [
        "//src/psm:analysis_headers",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# The signal-net EM switching-current engine (signal_em.h) depends only on
# em_signoff.h.
cc_test(
    name = "TestSignalEM",
    srcs = ["TestSignalEM.cpp"],
    deps = [
        "//src/psm:analysis_headers",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# The analysis-driven PDN sizing engine (pdn_sizing.h) is header-only; the gate
# additionally builds an independent Eigen ladder-network solve to check the
# closed-form rail droop against.
cc_test(
    name = "TestPdnSizing",
    srcs = ["TestPdnSizing.cpp"],
    deps = [
        "//src/psm:analysis_headers",
        "@eigen",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# The droop-driven decap sizer (decap_opt.h) is cross-checked against the
# transient MNA kernel, so this gate needs both include roots plus Eigen.
cc_test(
    name = "TestDecapOpt",
    srcs = ["TestDecapOpt.cpp"],
    deps = [
        "//src/psm:analysis_headers",
        "@eigen",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
