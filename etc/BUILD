load("@rules_python//python:defs.bzl", "py_library", "py_test")

package(features = ["layering_check"])

exports_files(
    [
        "whittle.py",
        "whittle_cleanup.tcl",
        "whittle_cut.tcl",
        "file_to_string.py",
    ],
    visibility = ["//visibility:public"],
)

exports_files(
    [
        "find_dup_ids.sh",
        "find_messages.py",
    ],
    visibility = ["//visibility:public"],
)

test_suite(
    name = "all_tests",
    tests = [
        ":code_coverage_test",
        ":release_regressions_test",
        ":test_whittle",
    ],
)

py_test(
    name = "code_coverage_test",
    size = "small",
    srcs = ["code_coverage_test.py"],
    data = ["CodeCoverage.sh"],
)

py_test(
    name = "test_whittle",
    srcs = ["test_whittle.py"],
    deps = [":whittle_lib"],
)

py_library(
    name = "whittle_lib",
    srcs = ["whittle.py"],
    imports = ["."],
)

py_test(
    name = "release_regressions_test",
    size = "small",
    srcs = [
        "check_release_regressions.py",
        "test_release_regressions.py",
    ],
    main = "test_release_regressions.py",
)
