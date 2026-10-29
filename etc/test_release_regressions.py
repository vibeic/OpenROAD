"""Check release selection and failure propagation without building OpenROAD."""

import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "release_regressions", Path(__file__).with_name("check_release_regressions.py")
)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ReleaseSelectionTest(unittest.TestCase):
    def invoke(self, *args, exits=(0,)):
        with (
            patch.object(runner.sys, "argv", ["check_release_regressions.py", *args]),
            patch.object(runner.shutil, "which", return_value="bazel"),
            patch.object(runner.subprocess, "run", side_effect=[
                SimpleNamespace(returncode=rc) for rc in exits
            ]) as run,
        ):
            rc = runner.main()
        return rc, [call.args[0] for call in run.call_args_list]

    def test_full_suite_remains_available(self):
        rc, calls = self.invoke()
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][-1], "//src/...")
        self.assertFalse(any("test_tag_filters" in a for a in calls[0]))

    def test_release_docs_do_not_filter_out_native_tests(self):
        rc, calls = self.invoke("--release-checks", exits=(0, 0))
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 2)
        self.assertIn("--test_tag_filters=doc_check", calls[0])
        self.assertIn("//src/...", calls[0])
        native = calls[1]
        self.assertNotIn("//src/...", native)
        self.assertFalse(any("test_tag_filters" in a for a in native))
        for target in (
            "//src/gpl/test:eco_freeze_neg-tcl_test",
            "//src/pdn/test:pads_short_strap-tcl_test",
            "//src/ram/test:make_8x8_2r1w_sky130-tcl_test",
            "//src/rsz/test:repair_hold_absolute_count-tcl_test",
            "//src/rcx/test:perpendicular_route_touching_iterm-tcl_test",
            "//src/upf/test:TestPowerSwitch",
            "//src/dbSta/test:test_read_verilog",
        ):
            self.assertIn(target, native)

    def test_documentation_failure_blocks_native_phase(self):
        rc, calls = self.invoke("--release-checks", exits=(7,))
        self.assertEqual(rc, 7)
        self.assertEqual(len(calls), 1)

    def test_native_failure_blocks_release(self):
        rc, calls = self.invoke("--release-checks", exits=(0, 9))
        self.assertEqual(rc, 9)
        self.assertEqual(len(calls), 2)

    def test_incompatible_scope_flags_are_refused(self):
        with self.assertRaises(SystemExit) as error:
            self.invoke("--release-checks", "--documentation-only")
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
