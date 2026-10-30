#!/usr/bin/env python3
"""Run the existing native Bazel regression suite before image publication."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


# These are the native failures observed and repaired in the 0.4.50 release
# investigation, plus the direct UPF/STA consumers of the adopted dependency.
# Documentation checks are selected separately, without filtering this list.
RELEASE_TARGETS = (
    "//src/ant/test:repair_antennas_no_guides-tcl_test",
    "//src/drt/test:ispd18_sample_reroute-tcl_test",
    "//src/gpl/test:eco_freeze-tcl_test",
    "//src/gpl/test:eco_freeze_neg-tcl_test",
    "//src/grt/test:repair_antennas_post_drt_above_max_layer_cugr-tcl_test",
    "//src/grt/test:repair_antennas_post_drt_below_min_cugr-tcl_test",
    "//src/grt/test:repair_antennas_post_drt_cugr-tcl_test",
    "//src/grt/test:repair_antennas_post_drt_relief_cugr-tcl_test",
    "//src/grt/test:repair_antennas_post_drt_relief_ndr_cugr-tcl_test",
    "//src/pdn/test:pads_short_strap-tcl_test",
    "//src/psm/test:em_lef_default_limits-tcl_test",
    "//src/psm/test:em_pad_ring_abutment-tcl_test",
    "//src/psm/test:gcd_em_drawn_width-tcl_test",
    "//src/psm/test:gcd_em_no_silent_skip-tcl_test",
    "//src/psm/test:gcd_em_via_per_cut-tcl_test",
    "//src/psm/test:gcd_transient_vectored-tcl_test",
    "//src/ram/test:make_8x8_2r1w_sky130-tcl_test",
    "//src/rcx/test:perpendicular_route_touching_bterm-tcl_test",
    "//src/rcx/test:perpendicular_route_touching_iterm-tcl_test",
    "//src/rsz/test:repair_hold_absolute_count-tcl_test",
    "//src/rsz/test:repair_setup_spef1-tcl_test",
    "//src/upf/test:TestPowerSwitch",
    "//src/dbSta/test:dbsta_unittest",
    "//src/dbSta/test:test_read_verilog",
    "//etc:release_regressions_test",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument(
        "--documentation-only", action="store_true",
        help="diagnose documentation checks without claiming full native coverage",
    )
    scope.add_argument(
        "--release-checks", action="store_true",
        help="run all documentation checks and the declared critical native release profile",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    bazel = (
        os.environ.get("BAZEL") or shutil.which("bazelisk") or shutil.which("bazel")
    )
    if not bazel:
        print(
            "Release regressions NOT_MEASURED: install Bazelisk/Bazel or set BAZEL.",
            file=sys.stderr,
        )
        return 2
    cache = Path(tempfile.gettempdir()) / "openroad-release-regressions"
    cache.mkdir(parents=True, exist_ok=True)
    command = [
        "nice", "-n", "10", bazel, "--batch",
        f"--output_user_root={cache / 'output'}",
        f"--host_jvm_args=-Djava.io.tmpdir={cache}",
        "test", "--jobs=4", "--local_test_jobs=4",
        "--local_resources=memory=16000",
        f"--repository_cache={cache / 'repositories'}",
        f"--disk_cache={cache / 'actions'}",
        "--build_tests_only", "--test_output=errors", "--keep_going",
    ]
    if args.release_checks:
        print(
            "OpenROAD regressions: release profile (all documentation checks + "
            f"{len(RELEASE_TARGETS)} declared release targets)", flush=True,
        )
        docs = subprocess.run(
            command + ["--test_tag_filters=doc_check", "//src/..."],
            cwd=root, check=False,
        )
        if docs.returncode:
            return docs.returncode
        return subprocess.run(
            command + list(RELEASE_TARGETS), cwd=root, check=False,
        ).returncode
    if args.documentation_only:
        command.append("--test_tag_filters=doc_check")
    command.append("//src/...")
    print(
        "OpenROAD regressions: "
        + ("documentation-only" if args.documentation_only else "complete native suite"),
        flush=True,
    )
    return subprocess.run(command, cwd=root, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
