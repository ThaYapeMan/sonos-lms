"""Run the production resume functions with simulated device/LMS boundaries."""
from pathlib import Path
import subprocess
import os
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "sonos-squeezebox.cpp").read_text()


def production_function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


# Extract, rather than duplicate, the production control flow. The fixture
# supplies only external state, network calls, and status/HTTP observations.
bodies = "\n\n".join(production_function(signature) for signature in (
    "static bool PlaySqueezeBoxLocked(unsigned stream_id, bool resetPosition)\n",
    'extern "C" void new_squeezebox_stream_id(',
    "static void dispatchDeferredStop(",
    'extern "C" void sonos_lms_transport(',
    "static void ObserveDeviceTransport(",
    "void ResumeSqueezeBox(",
    "void refreshStatus(",
))
with tempfile.TemporaryDirectory(prefix="sonos-device-resume-") as directory:
    directory = Path(directory)
    (directory / "production_resume.inc").write_text(bodies)
    executable = directory / "device-resume-test"
    subprocess.run([
        "g++", "-g", "-O2", "-Wall", "-Wextra", "-I", str(ROOT),
        "-I", str(directory), str(ROOT / "tests/device_resume_fixture.cpp"),
        "-o", str(executable), "-lpthread",
    ], check=True)
    for mode in ("stop", "pause"):
        subprocess.run([str(executable)], check=True,
                       env={**os.environ, "SONOS_SQUEEZEBOX_PAUSE": mode})
