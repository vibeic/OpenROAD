#!/usr/bin/env python3
"""Run the existing native Bazel regression suite before image publication."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--documentation-only", action="store_true",
        help="diagnose documentation checks without claiming full native coverage",
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
