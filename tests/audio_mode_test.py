"""Audio switch defaults, warnings, and read-once semantics."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonos-audio-mode-") as tmp:
    source = Path(tmp) / "settings.cpp"
    exe = Path(tmp) / "settings"
    source.write_text('''
#include "audio_mode.h"
#include <cassert>
int main(int argc, char** argv) {
    const bool stop = argv[1][0] == '1';
    assert((audioMode() == AudioMode::Legacy) == stop);
    setenv("SONOS_LMS_AUDIO", stop ? "24/48" : "16/44", 1);
    assert((audioMode() == AudioMode::Legacy) == stop);
}
''')
    subprocess.run(["g++", "-Wall", "-I", str(root), str(source), "-o", str(exe)], check=True)
    for value in (None, "24/48", "16/44", "invalid", ""):
        env = dict(os.environ)
        env.pop("SONOS_LMS_AUDIO", None)
        if value is not None:
            env["SONOS_LMS_AUDIO"] = value
        result = subprocess.run([str(exe), str(int(value == "16/44"))], env=env,
                                check=True, capture_output=True, text=True)
        assert result.stdout.count("Warning:") == (1 if value in ("invalid", "") else 0)
        assert f"SONOS_LMS_AUDIO={'16/44' if value == '16/44' else '24/48'}" in result.stdout
        print(f"PASS: audio mode {value!r}: parsing, startup log, read once")
