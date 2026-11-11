#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Runs SailTune's self-test of the pinned inventory checker on its throw-away fixture trees.
Cases that inspect SailTune's own repository (its desktop files, allowlist and build profiles) do
not apply to this tree and are left out by name; SailTube's tree is checked by inventory.py."""
import importlib.util
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PINNED = os.path.join(HERE, "..", "..", "Shared", "SailTune", "tests", "privacy", "test_check_network_inventory.py")
SAILTUNE_REPOSITORY_CASES = {
    "RealProfileTest": None,  # whole class
    "InventoryTest": {"test_real_repository_is_clean", "test_missing_baseline_in_real_allowlist_is_detected"},
}

spec = importlib.util.spec_from_file_location("test_check_network_inventory", PINNED)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

suite, skipped = unittest.TestSuite(), 0
seen = set()
for case in unittest.defaultTestLoader.loadTestsFromModule(module):
    for test in case:
        cls, name = type(test).__name__, test._testMethodName
        excluded = SAILTUNE_REPOSITORY_CASES.get(cls, set())
        if cls in SAILTUNE_REPOSITORY_CASES and (excluded is None or name in excluded):
            skipped += 1; seen.add((cls, name if excluded is not None else None)); continue
        suite.addTest(test)
missing = [(c, n) for c, names in SAILTUNE_REPOSITORY_CASES.items() for n in (names or [None]) if (c, n) not in seen]
if missing:
    print("privacy selftest: excluded cases no longer exist upstream: %s" % missing); sys.exit(1)
result = unittest.TextTestRunner(verbosity=0).run(suite)
print("privacy selftest: %d run, %d SailTune-repository cases left out" % (result.testsRun, skipped))
sys.exit(0 if result.wasSuccessful() else 1)
