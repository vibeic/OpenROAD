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

cc_library(
    name = "psm",
    srcs = [
        # vibeic fork file. Upstream migrated these targets to Bazel; a fork
        # file added to CMakeLists.txt alone is invisible to the build that
        # actually runs (see src/fin: `density_check.h file not found` from
        # bazel-out while the header sat in the repo and in CMake).
        "src/transient.h",
        "src/MakePDNSim.cpp",
        "src/connection.cpp",
        "src/connection.h",
        "src/debug_gui.cpp",
        "src/debug_gui.h",
        "src/heatMap.cpp",
        "src/heatMap.h",
        "src/ir_network.cpp",
        "src/ir_network.h",
        "src/ir_solver.cpp",
        "src/ir_solver.h",
        "src/node.cpp",
        "src/node.h",
        "src/pdnsim.cpp",
        "src/shape.cpp",
        "src/shape.h",
        ":swig",
        ":tcl",
    ],
    hdrs = [
        # vibeic fork file. Upstream migrated these targets to Bazel; a fork
        # file added to CMakeLists.txt alone is invisible to the build that
        # actually runs (see src/fin: `density_check.h file not found` from
        # bazel-out while the header sat in the repo and in CMake).
        "include/psm/decap_opt.h",
        "include/psm/em_signoff.h",
        "include/psm/pdn_sizing.h",
        "include/psm/signal_em.h",
        "include/psm/MakePDNSim.hh",
        "include/psm/pdnsim.h",
    ],
    includes = [
        "include",
        "src",
    ],
    deps = [
        "//:ord",
        "//src/dbSta",
        "//src/dbSta:dbNetwork",
        "//src/dpl",
        "//src/est",
        "//src/gui",
        "//src/odb/src/db",
        "//src/sta:opensta_lib",
        "//src/utl",
        "@boost.geometry",
        "@boost.polygon",
        "@boost.stacktrace",
        "@eigen",
        "@tcl_lang//:tcl",
    ],
)

# vibeic fork target.  The five analysis engines below are header-only and pull
# in nothing but Eigen, which is exactly why their CMake unit tests link no
# OpenDB/OpenSTA machinery.  Exposing them as their own header-only library lets
# the bazel unit tests keep that property: depending on ":psm" instead would
# make a second-scale gate require a full OpenROAD build, and a gate nobody can
# afford to run is a gate that does not run.
cc_library(
    name = "analysis_headers",
    hdrs = [
        "include/psm/decap_opt.h",
        "include/psm/em_signoff.h",
        "include/psm/pdn_sizing.h",
        "include/psm/signal_em.h",
        "src/transient.h",
    ],
    includes = [
        "include",
        "src",
    ],
    visibility = ["//:__subpackages__"],
    deps = ["@eigen"],
)

tcl_encode(
    name = "tcl",
    srcs = [
        "src/pdnsim.tcl",
    ],
    char_array_name = "psm_tcl_inits",
    namespace = "psm",
)

tcl_wrap_cc(
    name = "swig",
    srcs = [
        "src/pdnsim.i",
        "//:error_swig",
        "//src/sta:sta_swig_files",
    ],
    module = "psm",
    namespace_prefix = "psm",
    root_swig_src = "src/pdnsim.i",
    swig_includes = [
        "../sta",
        "src/psm/src",
    ],
    deps = [
        "//src/odb:swig",
    ],
)

python_wrap_cc(
    name = "swig-py",
    srcs = [
        "include/psm/pdnsim.h",
        "src/pdnsim-py.i",
        "//:error_swig-py",
    ],
    module = "psm_py",
    root_swig_src = "src/pdnsim-py.i",
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
        "src/pdnsim.tcl",
    ],
    visibility = ["//src/psm/test:__pkg__"],
)

messages_txt(
    visibility = [
        "//docs:__pkg__",
        "//src/psm/test:__pkg__",
    ],
)
