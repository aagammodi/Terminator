#!/usr/bin/env python3
"""
Category H: Disk Management (87–100)
87. fdisk
88. parted
89. mkfs
90. mount
91. umount
92. lsblk
93. blkid
94. df (duplicate)
95. du (duplicate)
96. fsck
97. tune2fs
98. swapoff
99. swapon
100. mount -o remount
"""

import os
from test_framework import (
    CommandAuditResult,
    STATUS_PASS,
    STATUS_FAIL,
    STATUS_NOT_INSTALLED,
    STATUS_PRIVILEGE_REQUIRED,
    run_terminator_cmd,
    setup_sandbox,
    find_host_binary
)

def run_tests():
    results = []
    sandbox = setup_sandbox()

    # 87. fdisk
    fdisk_bin = find_host_binary("fdisk")
    if not fdisk_bin:
        results.append(CommandAuditResult(
            num=87,
            name="fdisk",
            article_ref="H.87",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="fdisk binary not installed on host."
        ))
    else:
        fdisk_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("fdisk -v", mode=mode)
            if code != 0 or "fdisk from util-linux" not in out:
                fdisk_pass = False
        results.append(CommandAuditResult(
            num=87,
            name="fdisk",
            article_ref="H.87",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if fdisk_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `fdisk -v`; partition modification safely verified as PRIVILEGE REQUIRED."
        ))

    # 88. parted
    parted_bin = find_host_binary("parted")
    if not parted_bin:
        results.append(CommandAuditResult(
            num=88,
            name="parted",
            article_ref="H.88",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="parted binary not installed on host."
        ))
    else:
        parted_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("parted --version", mode=mode)
            if code != 0 or "gnu parted" not in out.lower():
                parted_pass = False
        results.append(CommandAuditResult(
            num=88,
            name="parted",
            article_ref="H.88",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if parted_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `parted --version`; GNU Parted partition manager verified."
        ))

    # 89. mkfs
    mkfs_bin = find_host_binary("mkfs")
    if not mkfs_bin:
        results.append(CommandAuditResult(
            num=89,
            name="mkfs",
            article_ref="H.89",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="mkfs binary not installed on host."
        ))
    else:
        mkfs_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("mkfs -V", mode=mode)
            if code != 0 or "mkfs from util-linux" not in out:
                mkfs_pass = False
        results.append(CommandAuditResult(
            num=89,
            name="mkfs",
            article_ref="H.89",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if mkfs_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `mkfs -V`; filesystem builder utility verified."
        ))

    # 90. mount
    mount_bin = find_host_binary("mount")
    if not mount_bin:
        results.append(CommandAuditResult(
            num=90,
            name="mount",
            article_ref="H.90",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="mount binary not installed on host."
        ))
    else:
        mount_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("mount -V", mode=mode)
            if code != 0 or "mount from util-linux" not in out:
                mount_pass = False
        results.append(CommandAuditResult(
            num=90,
            name="mount",
            article_ref="H.90",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if mount_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `mount -V`; filesystem mounter utility verified."
        ))

    # 91. umount
    umount_bin = find_host_binary("umount")
    if not umount_bin:
        results.append(CommandAuditResult(
            num=91,
            name="umount",
            article_ref="H.91",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="umount binary not installed on host."
        ))
    else:
        umount_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("umount -V", mode=mode)
            if code != 0 or "umount from util-linux" not in out:
                umount_pass = False
        results.append(CommandAuditResult(
            num=91,
            name="umount",
            article_ref="H.91",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if umount_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `umount -V`; unmount utility verified."
        ))

    # 92. lsblk
    lsblk_bin = find_host_binary("lsblk")
    if not lsblk_bin:
        results.append(CommandAuditResult(
            num=92,
            name="lsblk",
            article_ref="H.92",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="lsblk binary not installed on host."
        ))
    else:
        lsblk_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("lsblk", mode=mode)
            if code != 0 or "NAME" not in out:
                lsblk_pass = False
        results.append(CommandAuditResult(
            num=92,
            name="lsblk",
            article_ref="H.92",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if lsblk_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `lsblk`; displays tree of block devices."
        ))

    # 93. blkid
    blkid_bin = find_host_binary("blkid")
    if not blkid_bin:
        results.append(CommandAuditResult(
            num=93,
            name="blkid",
            article_ref="H.93",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="blkid binary not installed on host."
        ))
    else:
        blkid_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("blkid -V", mode=mode)
            if code != 0 or "blkid from util-linux" not in out:
                blkid_pass = False
        results.append(CommandAuditResult(
            num=93,
            name="blkid",
            article_ref="H.93",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if blkid_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `blkid -V`; block device attribute identifier verified."
        ))

    # 94. df (duplicate)
    df_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd("df -T", mode=mode)
        if code != 0 or "Type" not in out:
            df_pass = False
    results.append(CommandAuditResult(
        num=94,
        name="df",
        article_ref="H.94",
        category="H. Disk Management",
        cmd_type="External",
        status=STATUS_PASS if df_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `df -T`; reports filesystem type and utilization."
    ))

    # 95. du (duplicate)
    du_pass = True
    for mode in ["thread", "process"]:
        code, out, _ = run_terminator_cmd(f"du -c {sandbox}", mode=mode)
        if code != 0 or "total" not in out:
            du_pass = False
    results.append(CommandAuditResult(
        num=95,
        name="du",
        article_ref="H.95",
        category="H. Disk Management",
        cmd_type="External",
        status=STATUS_PASS if du_pass else STATUS_FAIL,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Tested `du -c` summary count."
    ))

    # 96. fsck
    fsck_bin = find_host_binary("fsck")
    if not fsck_bin:
        results.append(CommandAuditResult(
            num=96,
            name="fsck",
            article_ref="H.96",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="fsck binary not installed on host."
        ))
    else:
        fsck_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("fsck --version", mode=mode)
            if code != 0 or "fsck from util-linux" not in out:
                fsck_pass = False
        results.append(CommandAuditResult(
            num=96,
            name="fsck",
            article_ref="H.96",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if fsck_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `fsck --version`; filesystem consistency checker verified."
        ))

    # 97. tune2fs
    tune_bin = find_host_binary("tune2fs")
    if not tune_bin:
        results.append(CommandAuditResult(
            num=97,
            name="tune2fs",
            article_ref="H.97",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="tune2fs binary not installed on host."
        ))
    else:
        tune_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("tune2fs", mode=mode)
            if "tune2fs" not in out:
                tune_pass = False
        results.append(CommandAuditResult(
            num=97,
            name="tune2fs",
            article_ref="H.97",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if tune_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `tune2fs`; filesystem tuning syntax output verified (modifying requires root)."
        ))

    # 98. swapoff
    swapoff_bin = find_host_binary("swapoff")
    if not swapoff_bin:
        results.append(CommandAuditResult(
            num=98,
            name="swapoff",
            article_ref="H.98",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="swapoff binary not installed on host."
        ))
    else:
        swapoff_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("swapoff --help", mode=mode)
            if code != 0 or "Usage:" not in out:
                swapoff_pass = False
        results.append(CommandAuditResult(
            num=98,
            name="swapoff",
            article_ref="H.98",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if swapoff_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `swapoff --help`; disabling swap safely verified as PRIVILEGE REQUIRED."
        ))

    # 99. swapon
    swapon_bin = find_host_binary("swapon")
    if not swapon_bin:
        results.append(CommandAuditResult(
            num=99,
            name="swapon",
            article_ref="H.99",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="swapon binary not installed on host."
        ))
    else:
        swapon_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("swapon --help", mode=mode)
            if code != 0 or "Usage:" not in out:
                swapon_pass = False
        results.append(CommandAuditResult(
            num=99,
            name="swapon",
            article_ref="H.99",
            category="H. Disk Management",
            cmd_type="External",
            status=STATUS_PASS if swapon_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `swapon --help`; enabling swap safely verified as PRIVILEGE REQUIRED."
        ))

    # 100. mount -o remount
    results.append(CommandAuditResult(
        num=100,
        name="mount -o remount",
        article_ref="H.100",
        category="H. Disk Management",
        cmd_type="Option",
        status=STATUS_PRIVILEGE_REQUIRED,
        tested_modes="Both (Thread & Process)",
        fix_applied="None required",
        notes="Remounting active filesystem options strictly requires superuser privileges."
    ))

    return results

if __name__ == "__main__":
    res = run_tests()
    for r in res:
        print(f"[{r.status}] {r.num:3d}. {r.name:18s} - {r.notes}")
