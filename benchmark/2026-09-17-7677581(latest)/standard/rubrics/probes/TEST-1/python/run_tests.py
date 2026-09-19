# Coverage driver for TEST-1 (python3 -m trace runs this file).
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_probe  # noqa: E402

suite = unittest.defaultTestLoader.loadTestsFromModule(test_probe)
result = unittest.TextTestRunner(verbosity=2).run(suite)
print("passed=%d failed=%d" % (result.testsRun - len(result.failures) - len(result.errors), len(result.failures) + len(result.errors)))
