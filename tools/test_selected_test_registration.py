#!/usr/bin/env python3
"""Focused positive/negative checks for selected CTest pairing validation."""

import tempfile
import unittest
from pathlib import Path

from check_selected_test_registration import RegistrationError, validate_registration


class SelectedRegistrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.build = Path(self.temp.name)
        self.executable = self.build / "tests" / "agc_masked_select_tests"
        self.executable.parent.mkdir(parents=True)
        self.executable.write_bytes(b"synthetic executable marker")
        self.case = {"agc_masked_select": "agc_masked_select_tests"}
        self.ninja = "tests/agc_masked_select_tests: CXX_EXECUTABLE_LINKER__masked_select_Release\n"
        self.valid = {
            "tests": [
                {
                    "name": "agc_masked_select",
                    "command": [str(self.executable), "--test-arg"],
                }
            ]
        }

    def tearDown(self):
        self.temp.cleanup()

    def test_accepts_exact_built_command_pair(self):
        self.assertEqual(validate_registration(self.build, self.valid, self.ninja, self.case), 1)

    def test_rejects_missing_selected_test(self):
        with self.assertRaisesRegex(RegistrationError, "found 0"):
            validate_registration(self.build, {"tests": []}, self.ninja, self.case)

    def test_rejects_wrong_registered_executable(self):
        wrong = dict(self.valid)
        wrong["tests"] = [{"name": "agc_masked_select", "command": ["/tmp/wrong_tests"]}]
        with self.assertRaisesRegex(RegistrationError, "expected a built agc_masked_select_tests"):
            validate_registration(self.build, wrong, self.ninja, self.case)


if __name__ == "__main__":
    unittest.main()
