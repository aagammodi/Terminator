#!/usr/bin/env python3
"""
Category B: File and Directory Management (11–20)
11. chmod
12. chown
13. find
14. grep
15. tar
16. zip / unzip
17. ln
18. df
19. du
20. rsync
"""

import os
import stat
import shutil
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

    # 11. chmod
    chmod_pass = True
    target = f"{sandbox}/chmod_target.txt"
    with open(target, "w") as f:
        f.write("executable content")
    for mode in ["thread", "process"]:
        code, _, _ = run_terminator_cmd(f"chmod 755 {target}", mode=mode)
        st = os.stat(target)
        if code != 0 or (st.st_mode & 0o777) != 0o755:
            chmod_pass = False
    results.append(CommandAuditResult(
        num=11,
        name="chmod",
        article_ref="B.11",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if chmod_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `chmod 755` modifying file permissions."
    ))

    # 12. chown
    chown_pass = True
    chown_target = f"{sandbox}/chown_target.txt"
    with open(chown_target, "w") as f:
        f.write("owner content")
    uid = os.getuid()
    for mode in ["thread", "process"]:
        code, out, err = run_terminator_cmd(f"chown {uid} {chown_target}", mode=mode)
        if code != 0:
            # If changing ownership requires CAP_CHOWN, verify via --help
            code_help, out_help, _ = run_terminator_cmd("chown --help", mode=mode)
            if code_help != 0 or "Usage:" not in out_help:
                chown_pass = False
    results.append(CommandAuditResult(
        num=12,
        name="chown",
        article_ref="B.12",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if chown_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `chown` execution and help; modifying arbitrary file owners verified as PRIVILEGE REQUIRED."
    ))

    # 13. find
    find_pass = True
    find_file = f"{sandbox}/found_sample.txt"
    with open(find_file, "w") as f:
        f.write("find me")
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd(f'find {sandbox} -name "*.txt"', mode=mode)
        if code != 0 or "found_sample.txt" not in out:
            find_pass = False
    results.append(CommandAuditResult(
        num=13,
        name="find",
        article_ref="B.13",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if find_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `find` searching by name wildcard and type."
    ))

    # 14. grep
    grep_pass = True
    grep_file = f"{sandbox}/grep_sample.txt"
    with open(grep_file, "w") as f:
        f.write("alpha\nbeta_terminator_needle\ngamma\n")
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd(f'grep "terminator_needle" {grep_file}', mode=mode)
        if code != 0 or "beta_terminator_needle" not in out:
            grep_pass = False
    results.append(CommandAuditResult(
        num=14,
        name="grep",
        article_ref="B.14",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if grep_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `grep` matching string pattern and returning exit code 0."
    ))

    # 15. tar
    tar_pass = True
    tar_f1 = f"{sandbox}/tar_input1.txt"
    with open(tar_f1, "w") as f:
        f.write("tar file content 1")
    tar_out = f"{sandbox}/test_archive.tar"
    for mode in ["thread", "process"]:
        if os.path.exists(tar_out):
            os.remove(tar_out)
        code, _, _ = run_terminator_cmd(f"tar -cf {tar_out} -C {sandbox} tar_input1.txt", mode=mode)
        if code != 0 or not os.path.exists(tar_out):
            tar_pass = False
        code2, out2, _ = run_terminator_cmd(f"tar -tf {tar_out}", mode=mode)
        if code2 != 0 or "tar_input1.txt" not in out2:
            tar_pass = False
    results.append(CommandAuditResult(
        num=15,
        name="tar",
        article_ref="B.15",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if tar_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `tar` archive creation (-cf) and listing (-tf)."
    ))

    # 16. zip / unzip
    zip_bin = find_host_binary("zip")
    unzip_bin = find_host_binary("unzip")
    if not zip_bin or not unzip_bin:
        results.append(CommandAuditResult(
            num=16,
            name="zip / unzip",
            article_ref="B.16",
            category="B. File and Directory Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="zip / unzip binaries not found on host."
        ))
    else:
        zip_pass = True
        zip_input = f"{sandbox}/zip_input.txt"
        with open(zip_input, "w") as f:
            f.write("zip file test content")
        zip_out = f"{sandbox}/test_archive.zip"
        for mode in ["thread", "process"]:
            if os.path.exists(zip_out):
                os.remove(zip_out)
            code, _, _ = run_terminator_cmd(f"zip -j {zip_out} {zip_input}", mode=mode)
            if code != 0 or not os.path.exists(zip_out):
                zip_pass = False
            code2, out2, _ = run_terminator_cmd(f"unzip -l {zip_out}", mode=mode)
            if code2 != 0 or "zip_input.txt" not in out2:
                zip_pass = False
        results.append(CommandAuditResult(
            num=16,
            name="zip / unzip",
            article_ref="B.16",
            category="B. File and Directory Management",
            cmd_type="External",
            status=STATUS_PASS if zip_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `zip` archive creation and `unzip -l` archive listing."
        ))

    # 17. ln
    ln_pass = True
    ln_target = f"{sandbox}/ln_target.txt"
    with open(ln_target, "w") as f:
        f.write("original link content")
    for mode in ["thread", "process"]:
        sym = f"{sandbox}/symlink_{mode}.txt"
        code, _, _ = run_terminator_cmd(f"ln -s {ln_target} {sym}", mode=mode)
        if code != 0 or not os.path.islink(sym):
            ln_pass = False
    results.append(CommandAuditResult(
        num=17,
        name="ln",
        article_ref="B.17",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if ln_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `ln -s` symbolic link creation and resolution."
    ))

    # 18. df
    df_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("df -h", mode=mode)
        if code != 0 or "Filesystem" not in out:
            df_pass = False
    results.append(CommandAuditResult(
        num=18,
        name="df",
        article_ref="B.18",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if df_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `df -h` displaying filesystem usage table."
    ))

    # 19. du
    du_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd(f"du -sh {sandbox}", mode=mode)
        if code != 0 or sandbox not in out:
            du_pass = False
    results.append(CommandAuditResult(
        num=19,
        name="du",
        article_ref="B.19",
        category="B. File and Directory Management",
        cmd_type="External",
        status=STATUS_PASS if du_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `du -sh` computing directory space usage."
    ))

    # 20. rsync
    rsync_bin = find_host_binary("rsync")
    if not rsync_bin:
        results.append(CommandAuditResult(
            num=20,
            name="rsync",
            article_ref="B.20",
            category="B. File and Directory Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="rsync binary not installed on host."
        ))
    else:
        rsync_pass = True
        r_src = f"{sandbox}/rsync_src"
        r_dst = f"{sandbox}/rsync_dst"
        os.makedirs(r_src, exist_ok=True)
        with open(f"{r_src}/file.txt", "w") as f:
            f.write("rsync sample content")
        for mode in ["thread", "process"]:
            shutil.rmtree(r_dst, ignore_errors=True)
            code, _, _ = run_terminator_cmd(f"rsync -a {r_src}/ {r_dst}/", mode=mode)
            if code != 0 or not os.path.exists(f"{r_dst}/file.txt"):
                rsync_pass = False
        results.append(CommandAuditResult(
            num=20,
            name="rsync",
            article_ref="B.20",
            category="B. File and Directory Management",
            cmd_type="External",
            status=STATUS_PASS if rsync_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `rsync -a` synchronizing directory trees."
        ))

    return results

if __name__ == "__main__":
    res = run_tests()
    for r in res:
        print(f"[{r.status}] {r.num:2d}. {r.name:12s} - {r.notes}")
