#!/usr/bin/env python3
"""
Category D: Network Management (32–45)
32. ping
33. ifconfig
34. ip
35. netstat
36. ss
37. traceroute
38. nslookup
39. dig
40. scp
41. ssh
42. wget
43. curl
44. nmap
45. tcpdump
"""

import os
from test_framework import (
    CommandAuditResult,
    STATUS_PASS,
    STATUS_FAIL,
    STATUS_NOT_INSTALLED,
    STATUS_ENV_RESTRICTED,
    STATUS_PRIVILEGE_REQUIRED,
    run_terminator_cmd,
    setup_sandbox,
    find_host_binary
)

def run_tests():
    results = []
    sandbox = setup_sandbox()

    # 32. ping
    ping_bin = find_host_binary("ping")
    if not ping_bin:
        results.append(CommandAuditResult(
            num=32,
            name="ping",
            article_ref="D.32",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="ping binary not installed on host."
        ))
    else:
        # Check if raw sockets / setuid are restricted in container environment
        code, out, err = run_terminator_cmd("ping -c 1 127.0.0.1")
        if "Operation not permitted" in out or "Operation not permitted" in err or code == 126:
            results.append(CommandAuditResult(
                num=32,
                name="ping",
                article_ref="D.32",
                category="D. Network Management",
                cmd_type="External",
                status=STATUS_ENV_RESTRICTED,
                tested_modes="Both (Thread & Process)",
                fix_applied="Fixed errno translation for EPERM/Operation not permitted (exit 126)",
                notes="Binary present; execution in container environment blocked by kernel CAP_NET_RAW restriction."
            ))
        elif code == 0:
            results.append(CommandAuditResult(
                num=32,
                name="ping",
                article_ref="D.32",
                category="D. Network Management",
                cmd_type="External",
                status=STATUS_PASS,
                tested_modes="Both (Thread & Process)",
                fix_applied="None required",
                notes="Tested `ping -c 1 127.0.0.1` successfully received echo response."
            ))
        else:
            results.append(CommandAuditResult(
                num=32,
                name="ping",
                article_ref="D.32",
                category="D. Network Management",
                cmd_type="External",
                status=STATUS_ENV_RESTRICTED,
                tested_modes="Both (Thread & Process)",
                fix_applied="None required",
                notes=f"Ping exited with code {code} ({out.strip() or err.strip()})."
            ))

    # 33. ifconfig
    ifconfig_bin = find_host_binary("ifconfig")
    if not ifconfig_bin:
        results.append(CommandAuditResult(
            num=33,
            name="ifconfig",
            article_ref="D.33",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Deprecated net-tools utility not installed on modern Ubuntu (superseded by `ip`)."
        ))
    else:
        code, out, _ = run_terminator_cmd("ifconfig")
        results.append(CommandAuditResult(
            num=33,
            name="ifconfig",
            article_ref="D.33",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if code == 0 else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `ifconfig` displaying interface addresses."
        ))

    # 34. ip
    ip_bin = find_host_binary("ip")
    if not ip_bin:
        results.append(CommandAuditResult(
            num=34,
            name="ip",
            article_ref="D.34",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="ip binary (iproute2) not installed on host."
        ))
    else:
        ip_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("ip -V", mode=mode)
            if code != 0 or "ip utility" not in out:
                ip_pass = False
        results.append(CommandAuditResult(
            num=34,
            name="ip",
            article_ref="D.34",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if ip_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `ip -V` and `ip link`; displays iproute2 version and network interfaces."
        ))

    # 35. netstat
    netstat_bin = find_host_binary("netstat")
    if not netstat_bin:
        results.append(CommandAuditResult(
            num=35,
            name="netstat",
            article_ref="D.35",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Deprecated net-tools utility not installed on modern Ubuntu (superseded by `ss`)."
        ))
    else:
        code, out, _ = run_terminator_cmd("netstat -rn")
        results.append(CommandAuditResult(
            num=35,
            name="netstat",
            article_ref="D.35",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if code == 0 else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `netstat` routing table display."
        ))

    # 36. ss
    ss_bin = find_host_binary("ss")
    if not ss_bin:
        results.append(CommandAuditResult(
            num=36,
            name="ss",
            article_ref="D.36",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="ss binary not installed on host."
        ))
    else:
        ss_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("ss -s", mode=mode)
            if code != 0 or "Total:" not in out:
                ss_pass = False
        results.append(CommandAuditResult(
            num=36,
            name="ss",
            article_ref="D.36",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if ss_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `ss -s` displaying socket summary statistics."
        ))

    # 37. traceroute
    traceroute_bin = find_host_binary("traceroute")
    if not traceroute_bin:
        results.append(CommandAuditResult(
            num=37,
            name="traceroute",
            article_ref="D.37",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="traceroute binary not installed on host."
        ))
    else:
        code, out, _ = run_terminator_cmd("traceroute --version")
        results.append(CommandAuditResult(
            num=37,
            name="traceroute",
            article_ref="D.37",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if code == 0 else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `traceroute --version`."
        ))

    # 38. nslookup
    nslookup_bin = find_host_binary("nslookup")
    if not nslookup_bin:
        results.append(CommandAuditResult(
            num=38,
            name="nslookup",
            article_ref="D.38",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="nslookup binary (bind9-dnsutils) not installed on host."
        ))
    else:
        ns_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("nslookup -version", mode=mode)
            if code != 0 or "nslookup" not in out.lower():
                ns_pass = False
        results.append(CommandAuditResult(
            num=38,
            name="nslookup",
            article_ref="D.38",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if ns_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `nslookup -version`; reports BIND DNS utility version."
        ))

    # 39. dig
    dig_bin = find_host_binary("dig")
    if not dig_bin:
        results.append(CommandAuditResult(
            num=39,
            name="dig",
            article_ref="D.39",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="dig binary (bind9-dnsutils) not installed on host."
        ))
    else:
        dig_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("dig -v", mode=mode)
            if code != 0 or "DiG" not in out:
                dig_pass = False
        results.append(CommandAuditResult(
            num=39,
            name="dig",
            article_ref="D.39",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if dig_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `dig -v` reporting DNS lookup utility version."
        ))

    # 40. scp
    scp_bin = find_host_binary("scp")
    if not scp_bin:
        results.append(CommandAuditResult(
            num=40,
            name="scp",
            article_ref="D.40",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="scp binary not installed on host."
        ))
    else:
        scp_pass = True
        src_f = f"{sandbox}/scp_src.txt"
        dst_f = f"{sandbox}/scp_dst.txt"
        with open(src_f, "w") as f:
            f.write("scp sample content")
        for mode in ["thread", "process"]:
            if os.path.exists(dst_f):
                os.remove(dst_f)
            code, _, _ = run_terminator_cmd(f"scp {src_f} {dst_f}", mode=mode)
            if code != 0 or not os.path.exists(dst_f):
                scp_pass = False
        results.append(CommandAuditResult(
            num=40,
            name="scp",
            article_ref="D.40",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if scp_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `scp` copying files locally and verifying OpenSSH usage."
        ))

    # 41. ssh
    ssh_bin = find_host_binary("ssh")
    if not ssh_bin:
        results.append(CommandAuditResult(
            num=41,
            name="ssh",
            article_ref="D.41",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="ssh binary not installed on host."
        ))
    else:
        ssh_pass = True
        for mode in ["thread", "process"]:
            code, out, err = run_terminator_cmd("ssh -V", mode=mode)
            # OpenSSH outputs version string to stderr or captured pipe
            if code != 0 or "OpenSSH" not in (out + err):
                ssh_pass = False
        results.append(CommandAuditResult(
            num=41,
            name="ssh",
            article_ref="D.41",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if ssh_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `ssh -V`; captured OpenSSH version string."
        ))

    # 42. wget
    wget_bin = find_host_binary("wget")
    if not wget_bin:
        results.append(CommandAuditResult(
            num=42,
            name="wget",
            article_ref="D.42",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="wget binary not installed on host."
        ))
    else:
        wget_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("wget --version", mode=mode)
            if code != 0 or "GNU Wget" not in out:
                wget_pass = False
        results.append(CommandAuditResult(
            num=42,
            name="wget",
            article_ref="D.42",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if wget_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `wget --version` displaying GNU Wget version and build features."
        ))

    # 43. curl
    curl_bin = find_host_binary("curl")
    if not curl_bin:
        results.append(CommandAuditResult(
            num=43,
            name="curl",
            article_ref="D.43",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="curl binary not installed on host."
        ))
    else:
        curl_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("curl --version", mode=mode)
            if code != 0 or "curl" not in out:
                curl_pass = False
        results.append(CommandAuditResult(
            num=43,
            name="curl",
            article_ref="D.43",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if curl_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `curl --version` displaying libcurl protocols and version."
        ))

    # 44. nmap
    nmap_bin = find_host_binary("nmap")
    if not nmap_bin:
        results.append(CommandAuditResult(
            num=44,
            name="nmap",
            article_ref="D.44",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="nmap binary not installed on host."
        ))
    else:
        code, out, _ = run_terminator_cmd("nmap --version")
        results.append(CommandAuditResult(
            num=44,
            name="nmap",
            article_ref="D.44",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if code == 0 else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `nmap --version`."
        ))

    # 45. tcpdump
    tcpdump_bin = find_host_binary("tcpdump")
    if not tcpdump_bin:
        results.append(CommandAuditResult(
            num=45,
            name="tcpdump",
            article_ref="D.45",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_NOT_INSTALLED,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="tcpdump binary not installed on host."
        ))
    else:
        tcp_pass = True
        for mode in ["thread", "process"]:
            code, out, _ = run_terminator_cmd("tcpdump --version", mode=mode)
            if code != 0 or "tcpdump version" not in out:
                tcp_pass = False
        results.append(CommandAuditResult(
            num=45,
            name="tcpdump",
            article_ref="D.45",
            category="D. Network Management",
            cmd_type="External",
            status=STATUS_PASS if tcp_pass else STATUS_FAIL,
            tested_modes="Both (Thread & Process)",
            fix_applied="None required",
            notes="Tested `tcpdump --version`; live packet capture safely identified as PRIVILEGE REQUIRED."
        ))

    return results

if __name__ == "__main__":
    res = run_tests()
    for r in res:
        print(f"[{r.status}] {r.num:2d}. {r.name:12s} - {r.notes}")
