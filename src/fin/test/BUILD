load("@rules_python//python:defs.bzl", "py_test")

# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2022-2025, The OpenROAD Authors
load("//test:regression.bzl", "doc_check_test", "regression_test")

package(features = ["layering_check"])

# From CMakeLists.txt or_integration_tests(TESTS
COMPULSORY_TESTS = [
    "gcd_fill",
    "density_check_fixture",
    "density_window",
    "density_window_uncapped",
    "coupling_fill",
    "coupling_fill_off",
    "density_failsafe",
    "density_geometry_mismatch",
]

ALL_TESTS = COMPULSORY_TESTS

filegroup(
    name = "regression_resources",
    # Dependencies could be specified more narrowly per test case,
    # but at least it is not a glob of everything and there are
    # per test glob patterns below.
    srcs = [
        "fill.json",
        "gcd_prefill.def",
        "helpers.tcl",
        "sky130hd/sky130_fd_sc_hd__ff_n40C_1v95.lib",
        "sky130hd/sky130_fd_sc_hd__ss_n40C_1v40.lib",
        "sky130hd/sky130_fd_sc_hd__tt_025C_1v80.lib",
        "sky130hd/sky130_fd_sc_hd_merged.lef",
        "sky130hd/sky130hd.pdn.tcl",
        "sky130hd/sky130hd.rc",
        "sky130hd/sky130hd.rcx_rules",
        "sky130hd/sky130hd.tlef",
        "sky130hd/sky130hd.tracks",
        "sky130hd/sky130hd.vars",
        "sky130hd/sky130hd_multi_patterned.tlef",
        "sky130hd/sky130hd_std_cell.lef",
        "sky130hd/sky130hd_std_cell_vt.lef",
        "sky130hd/sky130hd_tt.lib",
        "sky130hd/sky130hd_vt.tlef",
        "sky130hd/work_around_yosys/formal_pdk.v",
    ],
)

[
    filegroup(
        name = test_name + "_resources",
        srcs = [":regression_resources"] + glob(
            [
                test_name + ".*",
            ],
        ) + {
            # Files a .tcl reads that the per-test glob does not reach: a
            # shared helper, a rule file, or a fixture .def owned by another
            # test.  Bazel runs the test in a sandbox, so anything not listed
            # here is simply absent at run time.
            "coupling_fill": [
                "coupling_helpers.tcl",
                "fill_met1.json",
            ],
            "coupling_fill_off": [
                "coupling_fill.def",
                "coupling_helpers.tcl",
                "fill_met1.json",
            ],
            "density_check_fixture": [
                "density_fixture.def",
            ],
            "density_failsafe": [
                "density_window.def",
                "fill_met1.json",
            ],
            "density_geometry_mismatch": [
                "density_window.def",
                "fill_met1.json",
            ],
            "density_window": [
                "fill_met1.json",
                "met1_band.txt",
            ],
            "density_window_uncapped": [
                "density_window.def",
                "fill_met1.json",
            ],
        }.get(test_name, []),
    )
    for test_name in ALL_TESTS
]

[
    regression_test(
        name = test_name,
        data = [":" + test_name + "_resources"],
        tags = [] if test_name in COMPULSORY_TESTS else ["manual"],
        visibility = ["//visibility:public"],
    )
    for test_name in ALL_TESTS
]

py_test(
    name = "fin_man_tcl_check",
    srcs = ["fin_man_tcl_check.py"],
    data = ["//src/fin:doc_files"],
    tags = ["doc_check"],
    deps = ["//docs/src/scripts:extract_utils"],
)

doc_check_test(
    name = "fin_readme_msgs_check",
    data = [
        "//src/fin:doc_files",
        "//src/fin:messages_txt",
    ],
    visibility = ["//visibility:public"],
)
