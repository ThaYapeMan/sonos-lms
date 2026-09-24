"""Test the production discovery parser/config reader without UDP network I/O."""
from pathlib import Path
import subprocess
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


bodies = "\n\n".join(production_function(signature) for signature in (
    "static bool parseDiscoveryResponse(",
    "static std::string readLmsServerFromConfig(",
))
with tempfile.TemporaryDirectory(prefix="sonos-discovery-") as directory:
    directory = Path(directory)
    (directory / "production_discovery.inc").write_text(bodies)
    executable = directory / "discovery-test"
    subprocess.run([
        "g++", "-g", "-O2", "-Wall", "-Wextra", "-I", str(directory),
        str(ROOT / "tests/lms_discovery_fixture.cpp"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable), str(directory / "config")], check=True)
