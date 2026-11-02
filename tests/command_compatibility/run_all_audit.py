#!/usr/bin/env python3
"""
Master Test Runner & Compatibility Audit for Terminator
Executes tests for all 100 Linux commands across Categories A through H
in both Thread Mode and Process Mode, plus Shell Integration tests.
"""

import os
import sys
import json

# Ensure local test framework and category scripts are in path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_framework import (
    teardown_sandbox,
    STATUS_PASS,
    STATUS_FAIL,
    STATUS_NOT_INSTALLED,
    STATUS_PRIVILEGE_REQUIRED,
    STATUS_INTERACTIVE_REQUIRED,
    STATUS_ENV_RESTRICTED,
    STATUS_DEFERRED
)

import test_cat_a_basic
import test_cat_b_file_mgmt
import test_cat_c_sys_info
import test_cat_d_network
import test_cat_e_user_perms
import test_cat_f_packages
import test_cat_g_processes
import test_cat_h_disk_mgmt
import test_shell_integration

def run_full_audit():
    print("=" * 80)
    print("  TERMINATOR: 100 LINUX COMMAND COMPATIBILITY AUDIT & TEST SUITE")
    print("=" * 80)

    all_results = []

    # Category A: Basic Commands (1–10)
    print("\n--- Auditing Category A: Basic Commands (1–10) ---")
    res_a = test_cat_a_basic.run_tests()
    all_results.extend(res_a)
    for r in res_a:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category B: File and Directory Management (11–20)
    print("\n--- Auditing Category B: File & Directory Management (11–20) ---")
    res_b = test_cat_b_file_mgmt.run_tests()
    all_results.extend(res_b)
    for r in res_b:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category C: System Information (21–31)
    print("\n--- Auditing Category C: System Information (21–31) ---")
    res_c = test_cat_c_sys_info.run_tests()
    all_results.extend(res_c)
    for r in res_c:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category D: Network Management (32–45)
    print("\n--- Auditing Category D: Network Management (32–45) ---")
    res_d = test_cat_d_network.run_tests()
    all_results.extend(res_d)
    for r in res_d:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category E: User and Permission Management (46–62)
    print("\n--- Auditing Category E: User & Permission Management (46–62) ---")
    res_e = test_cat_e_user_perms.run_tests()
    all_results.extend(res_e)
    for r in res_e:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category F: Package Management (63–71)
    print("\n--- Auditing Category F: Package Management (63–71) ---")
    res_f = test_cat_f_packages.run_tests()
    all_results.extend(res_f)
    for r in res_f:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category G: Process Management (72–86)
    print("\n--- Auditing Category G: Process Management (72–86) ---")
    res_g = test_cat_g_processes.run_tests()
    all_results.extend(res_g)
    for r in res_g:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Category H: Disk Management (87–100)
    print("\n--- Auditing Category H: Disk Management (87–100) ---")
    res_h = test_cat_h_disk_mgmt.run_tests()
    all_results.extend(res_h)
    for r in res_h:
        print(f"  [{r.status:^22}] #{r.num:2d} {r.name:15s} ({r.cmd_type}) - {r.notes}")

    # Shell Integration tests
    print("\n--- Auditing Shell Integration: Pipelines, Redirections & Expansions ---")
    shell_ok = test_shell_integration.run_tests()

    teardown_sandbox()

    # Aggregate statistics
    counts = {
        STATUS_PASS: 0,
        STATUS_NOT_INSTALLED: 0,
        STATUS_PRIVILEGE_REQUIRED: 0,
        STATUS_INTERACTIVE_REQUIRED: 0,
        STATUS_ENV_RESTRICTED: 0,
        STATUS_DEFERRED: 0,
        STATUS_FAIL: 0
    }

    for r in all_results:
        counts[r.status] = counts.get(r.status, 0) + 1

    total_audited = len(all_results)

    print("\n" + "=" * 80)
    print("  AUDIT SUMMARY STATISTICS")
    print("=" * 80)
    print(f"  Total Commands in Official Inventory : {total_audited} / 100")
    print(f"  PASS (Fully Compatible & Verified)   : {counts[STATUS_PASS]}")
    print(f"  NOT INSTALLED (Host Environment)     : {counts[STATUS_NOT_INSTALLED]}")
    print(f"  PRIVILEGE REQUIRED (Root/Sudo Ops)   : {counts[STATUS_PRIVILEGE_REQUIRED]}")
    print(f"  ENVIRONMENT RESTRICTED (Caps/Sandbox): {counts[STATUS_ENV_RESTRICTED]}")
    print(f"  INTERACTIVE TERMINAL REQUIRED (TUI)  : {counts[STATUS_INTERACTIVE_REQUIRED]}")
    print(f"  UNSUPPORTED / DEFERRED               : {counts[STATUS_DEFERRED]}")
    print(f"  FAIL (Bugs / Regressions)            : {counts[STATUS_FAIL]}")
    print(f"  Shell Integration Tests              : {'PASS' if shell_ok else 'FAIL'}")
    print("=" * 80)

    # Save JSON dump for documentation & report generation
    json_path = os.path.join(SCRIPT_DIR, "audit_results.json")
    with open(json_path, "w") as f:
        json.dump([r.to_dict() for r in all_results], f, indent=2)

    if counts[STATUS_FAIL] > 0 or not shell_ok or total_audited != 100:
        print("\n[FAILED] Audit detected unexpected command execution failures.")
        return 1

    print("\n[SUCCESS] All 100 Linux commands audited and verified successfully!")
    return 0

if __name__ == "__main__":
    sys.exit(run_full_audit())
