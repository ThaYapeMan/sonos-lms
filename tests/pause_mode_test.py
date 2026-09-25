"""Pause switch defaults, warnings, read-once semantics and strategy precedence."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonos-pause-mode-") as tmp:
    source = Path(tmp) / "settings.cpp"
    exe = Path(tmp) / "settings"
    source.write_text('''
#include "device_resume.h"
#include <cassert>
int main(int argc, char** argv) {
    const bool stop = argv[1][0] == '1';
    assert((pauseMode() == PauseMode::Stop) == stop);
    assert(deviceResumeStrategy() == (stop ? DeviceResume::SameURL503 : DeviceResume::FeedRestart));
    setenv("SONOS_SQUEEZEBOX_PAUSE", stop ? "pause" : "stop", 1);
    assert((pauseMode() == PauseMode::Stop) == stop);
    (void)deviceResumeStrategy();
}
''')
    subprocess.run(["g++", "-Wall", "-I", str(root), str(source), "-o", str(exe)], check=True)
    for value in (None, "pause", "stop", "invalid", ""):
        env = dict(os.environ, SONOS_SQUEEZEBOX_DEVICE_RESUME="feed-restart")
        env.pop("SONOS_SQUEEZEBOX_PAUSE", None)
        if value is not None:
            env["SONOS_SQUEEZEBOX_PAUSE"] = value
        result = subprocess.run([str(exe), str(int(value == "stop"))], env=env,
                                check=True, capture_output=True, text=True)
        assert result.stdout.count("Warning:") == (1 if value in ("invalid", "") else 0)
        assert result.stdout.count("ignored while") == (1 if value == "stop" else 0)
        print(f"PASS: pause mode {value!r}: parsing, startup log, read once, strategy precedence")
