# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025, The OpenROAD Authors

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:python_wrap_cc.bzl", "python_wrap_cc")
load("//bazel:tcl_encode_or.bzl", "tcl_encode")
load("//bazel:tcl_wrap_cc.bzl", "tcl_wrap_cc")
load("//test:regression.bzl", "messages_txt")

package(
    default_visibility = ["//:__subpackages__"],
    features = ["layering_check"],
)

# The metal density rule engine is header-only and dependency-free -- see
# test/cpp/CMakeLists.txt, which links the same correctness gate against
# nothing but gtest.  Publishing it as its own target lets the Bazel unit test
# keep that property instead of pulling in the whole :fin surface (ord, gui,
# odb, tcl) to read one header.  The header stays in :fin's `hdrs` too: Finale.h
# includes it, so every consumer of :fin must keep seeing it.
cc_library(
    name = "density_check_hdr",
    hdrs = ["include/fin/density_check.h"],
    includes = ["include"],
    visibility = ["//src/fin/test/cpp:__pkg__"],
)

cc_library(
    name = "fin",
    srcs = [
        # vibeic FL1 — metal density CHECK (the signoff half of the fill flow).
        # Listed here as well as in CMakeLists.txt: upstream migrated this
        # target to Bazel in the 2026-07-29 daily merge, and a fork file added
        # to the CMake source list alone becomes invisible to the build that
        # actually runs. The symptom was `Finale.h:8:10: fatal error:
        # 'fin/density_check.h' file not found` from bazel-out — our header was
        # in the repo, in CMake, and not in `hdrs`, so Bazel's sandbox never
        # saw it.
        "src/DensityCheck.cpp",
        "src/DensityCheck.h",
        "src/DensityFill.cpp",
        "src/DensityFill.h",
        "src/Finale.cpp",
        "src/MakeFinale.cpp",
        "src/graphics.cpp",
        "src/graphics.h",
        "src/polygon.h",
        ":swig",
        ":tcl",
    ],
    hdrs = [
        "include/fin/Finale.h",
        "include/fin/MakeFinale.h",
        # `Finale.h` includes this; a public header of ours must be a public
        # header of the target, or every consumer of `fin` fails to compile.
        "include/fin/density_check.h",
    ],
    includes = [
        "include",
    ],
    deps = [
        "//:ord",
        "//src/gui",
        "//src/odb/src/db",
        "//src/utl",
        "@boost.lexical_cast",
        "@boost.polygon",
        "@boost.property_tree",
        "@boost.stacktrace",
        "@tcl_lang//:tcl",
    ],
)

tcl_encode(
    name = "tcl",
    srcs = [
        "src/finale.tcl",
    ],
    char_array_name = "fin_tcl_inits",
    namespace = "fin",
)

tcl_wrap_cc(
    name = "swig",
    srcs = [
        "src/finale.i",
        "//:error_swig",
    ],
    module = "fin",
    namespace_prefix = "fin",
    root_swig_src = "src/finale.i",
    swig_includes = [
        "src/fin/src",
    ],
    deps = [
        "//src/odb:swig",
    ],
)

python_wrap_cc(
    name = "swig-py",
    srcs = [
        "include/fin/Finale.h",
        "src/finale-py.i",
        "//:error_swig-py",
    ],
    module = "fin_py",
    root_swig_src = "src/finale-py.i",
    swig_includes = [
        "include",
    ],
    deps = [
        "//src/odb:swig-py",
    ],
)

filegroup(
    name = "doc_files",
    srcs = [
        "README.md",
        "src/finale.tcl",
    ],
    visibility = ["//src/fin/test:__pkg__"],
)

messages_txt(
    visibility = [
        "//docs:__pkg__",
        "//src/fin/test:__pkg__",
    ],
)
