"""Exercise device-test host selection with fake unit and journal output."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "scripts/device-test.sh").read_text()
helpers = script[script.index("lms_from_exec_start() {"):script.index("# ------------------------------------------------------------------ main ---")]
assert "SCENARIOS=${SCENARIOS:-1 2 3 4 5 6 7}" in script
harness = r'''
set -uo pipefail
have() { return 0; }
mark() { printf '%s\n' "$*" >&2; return 1; } # logger failure must not erase a valid host
systemctl() {
    [[ $# == 4 && $1 == show && $2 == "$UNIT" && $3 == -p && $4 == ExecStart ]] || exit 97
    printf 'unit\n' >> "$CALLS"
    printf '%s\n' "$EXEC_START"
}
journalctl() {
    [[ $1 == -u && $2 == "$UNIT" ]] || exit 98
    if [[ $* == *'-n 500'* ]]; then
        printf 'recent\n' >> "$CALLS"
        printf '%s\n' "$RECENT"
    else
        [[ $# == 5 && $3 == --no-pager && $4 == -o && $5 == cat ]] || exit 99
        printf 'full\n' >> "$CALLS"
        printf '%s\n' "$FULL"
    fi
}
if resolve_lms_host "$CONFIG"; then
    printf '%s|%s\n' "$LMS" "$LMS_SOURCE"
else
    exit 1
fi
'''
with tempfile.TemporaryDirectory(prefix="sonos-device-script-") as tmp:
    config = Path(tmp) / "config"
    calls = Path(tmp) / "calls"
    cases = [
        ("explicit override", "override", "LMS_SERVER=config", "recent", "--server=unit", "full", "override|LMS override", []),
        ("config", "", "LMS_SERVER=config", "recent", "--server=unit", "full", "config|config LMS_SERVER", []),
        ("recent journal", "", "", "recent", "--server=unit", "full", "recent|recent unit journal", ["recent"]),
        ("unit equals", "", "", "", "--server=192.168.178.23", "full", "192.168.178.23|unit ExecStart --server", ["recent", "unit"]),
        ("unit separate", "", "", "", "--room Study --server lms.local", "full", "lms.local|unit ExecStart --server", ["recent", "unit"]),
        ("unit quoted host", "", "", "", '--server="lms.local"', "full", "lms.local|unit ExecStart --server", ["recent", "unit"]),
        ("full journal", "", "", "", "--room Study", "old-lms", "old-lms|full unit journal", ["recent", "unit", "full"]),
        ("missing server value", "", "", "", "--server --room Study", "old-lms", "old-lms|full unit journal", ["recent", "unit", "full"]),
        ("unknown host", "", "", "", "--room Study", "", None, ["recent", "unit", "full"]),
    ]
    for label, override, contents, recent, unit, full, expected, sources in cases:
        config.write_text(contents + "\n")
        calls.write_text("")
        env = {**os.environ, "LMS": override, "UNIT": "sonos-lms@Study Room.service",
               "CONFIG": str(config), "CALLS": str(calls),
               "EXEC_START": "ExecStart={ path=/bridge ; argv[]=/bridge " + unit + " ; ignore_errors=no ; }",
               "RECENT": "LMS server from discovery: " + recent if recent else "unrelated status output",
               "FULL": "LMS server from discovery: obsolete\nLMS server from discovery: " + full if full else "unrelated status output"}
        result = subprocess.run(["bash", "-c", helpers + harness], env=env, capture_output=True, text=True)
        assert result.returncode == (0 if expected else 1), (label, result.stderr)
        assert result.stdout.strip() == (expected or ""), (label, result.stdout)
        assert calls.read_text().splitlines() == sources, (label, calls.read_text())
        if expected:
            host, source = expected.split("|")
            assert f"LMS host {host} (source: {source})" in result.stderr
        print(f"PASS: device-test LMS {label}, fallback order and source logging")
subprocess.run(["bash", "-n", str(ROOT / "scripts/device-test.sh")], check=True)
print("PASS: device-test syntax and default scenarios 1–7")
