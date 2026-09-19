# TEST-1 probe, Python. Exactly 3 tests: two assert a true condition, one asserts a false condition.
import unittest


def add(a, b):
    return a + b


class ProbeTests(unittest.TestCase):
    def test_pass_one(self):
        self.assertTrue(add(1, 1) == 2)

    def test_pass_two(self):
        self.assertTrue(add(2, 3) == 5)

    def test_fail_one(self):
        self.assertTrue(add(1, 1) == 3)
