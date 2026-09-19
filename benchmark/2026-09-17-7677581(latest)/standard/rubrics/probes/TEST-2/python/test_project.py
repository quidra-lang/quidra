import unittest

import mod_a
import mod_b


class ProjectTests(unittest.TestCase):
    def test_scale(self):
        self.assertEqual(mod_a.scale(2), 6)

    def test_scale_and_offset(self):
        self.assertEqual(mod_b.scale_and_offset(2), 7)
