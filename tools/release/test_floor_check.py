# SPDX-License-Identifier: GPL-3.0-or-later
"""floor_check: a release can't leave sold boards without their version floor (W-1156).

    python3 -m unittest tools/release/test_floor_check.py
"""
import pathlib, sys, unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import floor_check  # noqa: E402


class Floor(unittest.TestCase):
    def test_the_first_release_with_sign_in_is_the_floor(self):
        self.assertEqual(floor_check.problems("v0.5.0", "v0.5.0", ["v0.2.0", "v0.3.0", "v0.4.0"]), [])

    def test_a_release_below_the_floor_is_refused(self):
        p = floor_check.problems("v0.4.1", "v0.5.0", ["v0.4.0"])
        self.assertEqual(len(p), 1)
        self.assertIn("v0.4.1 is below", p[0])

    def test_a_floor_at_or_below_a_release_without_sign_in_is_refused(self):
        # v0.5.0 went out before sign-in: a board on v0.6.0 could go back to it, open.
        p = floor_check.problems("v0.6.0", "v0.5.0", ["v0.4.0", "v0.5.0"])
        self.assertEqual(len(p), 1)
        self.assertIn("v0.5.0 has no sign-in", p[0])

    def test_later_releases_with_sign_in_pass(self):
        # Tags with sign-in aren't in the list: only those without it are.
        self.assertEqual(floor_check.problems("v0.7.0", "v0.5.0", ["v0.2.0", "v0.4.0"]), [])

    def test_reads_the_floor_from_the_header(self):
        self.assertRegex(floor_check.floor_of(floor_check.HEADER.read_text()), r"^v\d+\.\d+\.\d+$")


if __name__ == "__main__":
    unittest.main()
