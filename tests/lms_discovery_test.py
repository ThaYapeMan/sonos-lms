"""Test the production discovery parser/config reader without UDP network I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "sonos-lms.cpp").read_text()


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

# Compile the actual command with a discovery stub: no sockets or bridge startup.
body = production_function("static int findServerCommand(")
assert source.index('if (findFlag(argc, argv, "--find-server"))') < source.index('(void)pauseMode();', source.index('int main('))
with tempfile.TemporaryDirectory(prefix="sonos-find-server-") as directory:
    directory = Path(directory)
    fixture = directory / "command.cpp"
    fixture.write_text('#include <string>\n#include <cstdio>\n#include <cassert>\n'
        'static std::string answer;\n'
        'static std::string discoverLmsServer(unsigned timeout, bool quiet) { '
        'assert(timeout == 3000 && quiet); return answer; }\n' + body +
        '\nint main(int argc, char** argv) { if (argc > 1) answer = argv[1]; return findServerCommand(); }\n')
    executable = directory / "command"
    subprocess.run(['g++', '-Wall', '-Wextra', str(fixture), '-o', str(executable)], check=True)
    for host in ('192.0.2.10', ''):
        result = subprocess.run([str(executable), host], capture_output=True, text=True)
        assert result.returncode == (0 if host else 2)
        assert result.stdout == (host + '\n' if host else '') and not result.stderr
    print('PASS: --find-server stdout, exit codes and quiet discovery without UDP I/O')
