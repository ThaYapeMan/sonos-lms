"""Check startup defaults, invalid values, and read-once settings in fresh processes."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonos-resume-response-") as tmp:
    source = Path(tmp) / "settings.cpp"
    exe = Path(tmp) / "settings"
    source.write_text('''
#include "resume_response.h"
#include <cassert>
int main(int argc, char** argv) {
    const auto initial = resumeResponseSettings();
    assert(initial.frames == (argv[1][0] == '1'));
    assert(initial.raw == (argv[2][0] == '1'));
    setenv("SONOS_SQUEEZEBOX_RESUME_BODY", initial.frames ? "header" : "frames", 1);
    setenv("SONOS_SQUEEZEBOX_RESUME_TRANSFER", initial.raw ? "chunked" : "raw", 1);
    assert(resumeResponseSettings().frames == initial.frames);
    assert(resumeResponseSettings().raw == initial.raw);
}
''')
    subprocess.run(["g++", "-Wall", "-I", str(root), str(source), "-o", str(exe)], check=True)
    for body, transfer in [(None, None), ("header", "chunked"), ("frames", "raw"),
                           ("header", "raw"), ("frames", "chunked"), ("bad", "bad"), ("", "")]:
        env = {k: v for k, v in os.environ.items() if k not in (
            "SONOS_SQUEEZEBOX_RESUME_BODY", "SONOS_SQUEEZEBOX_RESUME_TRANSFER")}
        for key, value in [("BODY", body), ("TRANSFER", transfer)]:
            if value is not None:
                env["SONOS_SQUEEZEBOX_RESUME_" + key] = value
        result = subprocess.run([str(exe), str(int(body == "frames")), str(int(transfer == "raw"))],
                                env=env, check=True, text=True, capture_output=True)
        assert result.stdout.count("Warning:") == (2 if body in ("bad", "") else 0)
        assert "SONOS_SQUEEZEBOX_RESUME_BODY=" in result.stdout
        assert "SONOS_SQUEEZEBOX_RESUME_TRANSFER=" in result.stdout
        print(f"PASS: startup/read-once settings body={body!r} transfer={transfer!r}")
