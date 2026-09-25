"""Exercise the production C shutdown with a pump still accessing its buffer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "output_sonos.c").read_text()
start = source.index("void output_close_sonos(void)\n")
opening = source.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
close = source[start:end]
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
static atomic_bool pump_running = true, exited = false;
static bool pump_started = false, freed = false, closed = false;
static pthread_t pump_thread;
static unsigned pcm_staged_frames = 1;
static unsigned char* pcm_staging;
#define LOG_INFO(...) ((void)0)
static void checked_free(void* pointer) {
    assert(atomic_load(&exited));
    assert(!pump_started);
    freed = true;
    free(pointer);
}
static void output_close_common(void) {
    assert(freed && !pcm_staging && pcm_staged_frames == 0);
    closed = true;
}
#define free checked_free
''' + close + r'''
#undef free
static void* pump(void* unused) {
    while (atomic_load(&pump_running)) usleep(1000);
    // Signal alone is insufficient: the exiting pump still owns the buffer.
    usleep(20000);
    pcm_staging[0] = 42;
    atomic_store(&exited, true);
    return NULL;
}
int main(void) {
    pcm_staging = malloc(1);
    assert(pthread_create(&pump_thread, NULL, pump, NULL) == 0);
    pump_started = true;
    output_close_sonos();
    assert(closed && freed && !atomic_load(&pump_running));
    puts("PASS: output shutdown joins the pump before freeing staging/common buffers");
}
'''
with tempfile.TemporaryDirectory(prefix="sonos-output-shutdown-") as tmp:
    path = Path(tmp) / "shutdown.c"
    executable = Path(tmp) / "shutdown"
    path.write_text(fixture)
    subprocess.run(["gcc", "-std=gnu11", "-Wall", str(path), "-o", str(executable), "-lpthread"], check=True)
    subprocess.run([str(executable)], check=True, timeout=5)
