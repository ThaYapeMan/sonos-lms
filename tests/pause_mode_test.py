"""Pause switch defaults, warnings, and read-once semantics."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonos-pause-mode-") as tmp:
    source = Path(tmp) / "settings.cpp"
    exe = Path(tmp) / "settings"
    source.write_text('''
#include "pause_mode.h"
#include <cassert>
int main(int argc, char** argv) {
    const bool stop = argv[1][0] == '1';
    assert((pauseMode() == PauseMode::Stop) == stop);
    setenv("SONOS_SQUEEZEBOX_PAUSE", stop ? "pause" : "stop", 1);
    assert((pauseMode() == PauseMode::Stop) == stop);
}
''')
    subprocess.run(["g++", "-Wall", "-I", str(root), str(source), "-o", str(exe)], check=True)
    for value in (None, "pause", "stop", "invalid", ""):
        env = dict(os.environ)
        env.pop("SONOS_SQUEEZEBOX_PAUSE", None)
        if value is not None:
            env["SONOS_SQUEEZEBOX_PAUSE"] = value
        result = subprocess.run([str(exe), str(int(value != "pause"))], env=env,
                                check=True, capture_output=True, text=True)
        assert result.stdout.count("Warning:") == (1 if value in ("invalid", "") else 0)
        print(f"PASS: pause mode {value!r}: parsing, startup log, read once")
