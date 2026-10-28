#!/usr/bin/env python3
"""Run the registered documentation checks before a fork/image release."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    bazel = (
        os.environ.get("BAZEL") or shutil.which("bazelisk") or shutil.which("bazel")
    )
    if not bazel:
        print(
            "Documentation checks NOT_MEASURED: install Bazelisk/Bazel or set BAZEL.",
            file=sys.stderr,
        )
        return 2
    cache = Path(tempfile.gettempdir()) / "openroad-documentation-checks"
    cache.mkdir(parents=True, exist_ok=True)
    command = [
        "nice", "-n", "10", bazel, "--batch",
        f"--output_user_root={cache / 'output'}",
        f"--host_jvm_args=-Djava.io.tmpdir={cache}",
        "test", "--jobs=4", "--local_test_jobs=4",
        "--local_resources=memory=16000",
        f"--repository_cache={cache / 'repositories'}",
        f"--disk_cache={cache / 'actions'}",
        "--build_tests_only", "--test_tag_filters=doc_check",
        "--test_output=errors", "--keep_going", "//src/...",
    ]
    return subprocess.run(command, cwd=root, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
