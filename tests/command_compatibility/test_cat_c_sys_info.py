#!/usr/bin/env python3
"""
Category C: System Information (21–31)
21. uname
22. top
23. htop
24. df (duplicate)
25. du (duplicate)
26. free
27. uptime
28. hostname
29. uname -r
30. vmstat
31. iostat
"""

import os
from test_framework import (
    CommandAuditResult,
    STATUS_PASS,
    STATUS_FAIL,
    STATUS_NOT_INSTALLED,
    run_terminator_cmd,
    setup_sandbox,
    find_host_binary
)

def run_tests():
    results = []
    sandbox = setup_sandbox()

    # 21. uname
    uname_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("uname -a", mode=mode)
        if code != 0 or "Linux" not in out:
            uname_pass = False
    results.append(CommandAuditResult(
        num=21,
        name="uname",
        article_ref="C.21",
        category="C. System Information",
        cmd_type="External",
        status=STATUS_PASS if uname_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `uname -a`; reports kernel name, host, release and architecture."
    ))

    # 22. top
    top_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("top -b -n 1", mode=mode)
        if code != 0 or "load average" not in out:
            top_pass = False
    results.append(CommandAuditResult(
        num=22,
        name="top",
        article_ref="C.22",
        category="C. System Information",
        cmd_type="External",
        status=STATUS_PASS if top_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="Fixed TUI interceptor to permit batch flag -b",
        notes="Tested `top -b -n 1` in batch streaming mode; outputs process header and CPU metrics."
    ))

    # 23. htop
    htop_bin = find_host_binary("htop")
    if not htop_bin:
        results.append(CommandAuditResult(
            num=23,
            name="htop",
            article_ref="C.23",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="htop binary not installed on host."
        ))
    else:
        htop_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("htop --version", mode=mode)
            if code != 0 or "htop" not in out.lower():
                htop_pass = False
        results.append(CommandAuditResult(
            num=23,
            name="htop",
            article_ref="C.23",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_PASS if htop_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="Fixed is_tui_command to permit non-interactive --version query",
            notes="Tested `htop --version`; reports version without freezing GUI."
        ))

    # 24. df (duplicate)
    df_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("df -k", mode=mode)
        if code != 0 or "1K-blocks" not in out:
            df_pass = False
    results.append(CommandAuditResult(
        num=24,
        name="df",
        article_ref="C.24",
        category="C. System Information",
        cmd_type="External",
        status=STATUS_PASS if df_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `df -k` in 1K-block formatting."
    ))

    # 25. du (duplicate)
    du_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd(f"du -s {sandbox}", mode=mode)
        if code != 0 or sandbox not in out:
            du_pass = False
    results.append(CommandAuditResult(
        num=25,
        name="du",
        article_ref="C.25",
        category="C. System Information",
        cmd_type="External",
        status=STATUS_PASS if du_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `du -s` directory summary."
    ))

    # 26. free
    free_bin = find_host_binary("free")
    if not free_bin:
        results.append(CommandAuditResult(
            num=26,
            name="free",
            article_ref="C.26",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="free binary not installed on host."
        ))
    else:
        free_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("free -m", mode=mode)
            if code != 0 or "Mem:" not in out:
                free_pass = False
        results.append(CommandAuditResult(
            num=26,
            name="free",
            article_ref="C.26",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_PASS if free_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `free -m` displaying memory and swap in mebibytes."
        ))

    # 27. uptime
    uptime_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("uptime", mode=mode)
        if code != 0 or "load average" not in out:
            uptime_pass = False
    results.append(CommandAuditResult(
        num=27,
        name="uptime",
        article_ref="C.27",
        category="C. System Information",
        cmd_type="External",
        status=STATUS_PASS if uptime_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `uptime` displaying system running time and load averages."
    ))

    # 28. hostname
    hostname_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("hostname", mode=mode)
        if code != 0 or len(out.strip()) == 0:
            hostname_pass = False
    results.append(CommandAuditResult(
        num=28,
        name="hostname",
        article_ref="C.28",
        category="C. System Information",
        cmd_type="External",
        status=STATUS_PASS if hostname_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `hostname` printing current system host name."
    ))

    # 29. uname -r
    uname_r_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("uname -r", mode=mode)
        if code != 0 or len(out.strip()) == 0:
            uname_r_pass = False
    results.append(CommandAuditResult(
        num=29,
        name="uname -r",
        article_ref="C.29",
        category="C. System Information",
        cmd_type="Option",
        status=STATUS_PASS if uname_r_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `uname -r` returning kernel release string."
    ))

    # 30. vmstat
    vmstat_bin = find_host_binary("vmstat")
    if not vmstat_bin:
        results.append(CommandAuditResult(
            num=30,
            name="vmstat",
            article_ref="C.30",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="vmstat binary not installed on host."
        ))
    else:
        vmstat_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("vmstat 1 1", mode=mode)
            if code != 0 or "procs" not in out:
                vmstat_pass = False
        results.append(CommandAuditResult(
            num=30,
            name="vmstat",
            article_ref="C.30",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_PASS if vmstat_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `vmstat 1 1` reporting virtual memory statistics."
        ))

    # 31. iostat
    iostat_bin = find_host_binary("iostat")
    if not iostat_bin:
        results.append(CommandAuditResult(
            num=31,
            name="iostat",
            article_ref="C.31",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="iostat binary (sysstat package) not installed on host."
        ))
    else:
        iostat_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("iostat -c 1 1", mode=mode)
            if code != 0 or "avg-cpu:" not in out:
                iostat_pass = False
        results.append(CommandAuditResult(
            num=31,
            name="iostat",
            article_ref="C.31",
            category="C. System Information",
            cmd_type="External",
            status=STATUS_PASS if iostat_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `iostat -c 1 1` reporting CPU statistics."
        ))

    return results

if __name__ == "__main__":
    res = run_tests()
    for r in res:
        print(f"[{r.status}] {r.num:2d}. {r.name:12s} - {r.notes}")
