#!/usr/bin/env python3
"""
Terminator Linux Command Compatibility Test Framework
Provides execution harnesses for Thread Mode and Process Mode, sandbox management,
and standardized status recording for the 100 Linux commands.
"""

import os
import sys
import shutil
import subprocess
import tempfile

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
TERMINATOR_BIN = os.path.join(PROJECT_ROOT, "terminator")
SANDBOX_DIR = "/tmp/terminator_cmd_sandbox"

STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_NOT_INSTALLED = "NOT INSTALLED"
STATUS_PRIVILEGE_REQUIRED = "PRIVILEGE REQUIRED"
STATUS_INTERACTIVE_REQUIRED = "INTERACTIVE TERMINAL REQUIRED"
STATUS_ENV_RESTRICTED = "ENVIRONMENT RESTRICTED"
STATUS_DEFERRED = "UNSUPPORTED / DEFERRED"

VALID_STATUSES = {
    STATUS_PASS,
    STATUS_FAIL,
    STATUS_NOT_INSTALLED,
    STATUS_PRIVILEGE_REQUIRED,
    STATUS_INTERACTIVE_REQUIRED,
    STATUS_ENV_RESTRICTED,
    STATUS_DEFERRED,
}

class CommandAuditResult:
    def __init__(self, num, name, article_ref, category, cmd_type, status, tested_modes, fix_applied, notes):
        self.num = num
        self.name = name
        self.article_ref = article_ref
        self.category = category
        self.cmd_type = cmd_type
        self.status = status
        self.tested_modes = tested_modes
        self.fix_applied = fix_applied
        self.notes = notes

    def to_dict(self):
        return {
            "num": self.num,
            "name": self.name,
            "article_ref": self.article_ref,
            "category": self.category,
            "cmd_type": self.cmd_type,
            "status": self.status,
            "tested_modes": self.tested_modes,
            "fix_applied": self.fix_applied,
            "notes": self.notes
        }

def setup_sandbox():
    """Ensure a clean, isolated directory for file manipulation tests."""
    if os.path.exists(SANDBOX_DIR):
        shutil.rmtree(SANDBOX_DIR, ignore_errors=True)
    os.makedirs(SANDBOX_DIR, exist_ok=True)
    return SANDBOX_DIR

def teardown_sandbox():
    """Clean up the test sandbox."""
    if os.path.exists(SANDBOX_DIR):
        shutil.rmtree(SANDBOX_DIR, ignore_errors=True)

def find_host_binary(cmd_name):
    """Locate binary in PATH or standard system binary paths."""
    p = shutil.which(cmd_name)
    if p:
        return p
    for sdir in ["/usr/sbin", "/sbin", "/usr/local/sbin", "/usr/games"]:
        candidate = os.path.join(sdir, cmd_name)
        if os.path.exists(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None

def run_terminator_cmd(cmd_str, mode="thread", cwd=None, timeout=5):
    """
    Execute a command line via `./terminator -c "<cmd_str>"` or `./terminator useProcess=True -c "<cmd_str>"`.
    Returns (returncode, stdout, stderr).
    """
    if not os.path.exists(TERMINATOR_BIN):
        raise FileNotFoundError(f"Terminator binary not found at {TERMINATOR_BIN}. Run `make` first.")

    args = [TERMINATOR_BIN]
    if mode == "process":
        args.append("useProcess=True")
    args.extend(["-c", cmd_str])

    env = os.environ.copy()
    env["TERMINATOR_HEADLESS"] = "1"

    try:
        proc = subprocess.run(
            args,
            cwd=cwd or PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Execution timed out"

def run_terminator_session(input_lines, mode="thread", cwd=None, timeout=5):
    """
    Feed multi-command session input via stdin to `./terminator --test`.
    Returns (returncode, stdout, stderr).
    """
    if not os.path.exists(TERMINATOR_BIN):
        raise FileNotFoundError(f"Terminator binary not found at {TERMINATOR_BIN}. Run `make` first.")

    args = [TERMINATOR_BIN]
    if mode == "process":
        args.append("useProcess=True")
    args.append("--test")

    env = os.environ.copy()
    env["TERMINATOR_HEADLESS"] = "1"

    stdin_data = "\n".join(input_lines) + "\nexit\n"

    try:
        proc = subprocess.run(
            args,
            input=stdin_data,
            cwd=cwd or PROJECT_ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Execution timed out"
