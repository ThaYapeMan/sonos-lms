#include "upnp/own_speaker_control.h"
#include "sonos-status.h"
#include "resume_state.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <mutex>
static std::shared_ptr<upnp::SpeakerControl> gPlayer;
static std::atomic<bool> ourStreamStarted{true};
static std::atomic<unsigned> streamId{6};
static ResumeState resumeState;
static std::mutex resumeMutex;
static int gServer = 0, gMac = 0;
static unsigned cliPlays = 0, cliPauses = 0, ended = 0;
static bool stream_just_restarted() { return false; }
static bool squeezebox_response_open(unsigned) { return false; }
static void end_squeezebox_response() { ++ended; }
static void hold_squeezebox_resume(unsigned) {}
static bool sendLmsCommand(int, int, const char* command) {
    if (std::string(command) == "play") ++cliPlays;
    if (std::string(command) == "pause 1") ++cliPauses;
    return true;
}
void ResumeSqueezeBox(unsigned);
static void note_squeezebox_device_close() {}
#include "production_own_poll.inc"
int main(int argc, char** argv) {
    assert(argc == 2);
    gPlayer = std::make_shared<upnp::OwnSpeakerControl>([] { return 1450; }, std::strtoul(argv[1], nullptr, 10));
    assert(gPlayer->discover("Study", "127.0.0.1"));
    bridge::Status status(gPlayer);
    resumeState.command('s');
    refreshStatus(status); // PLAYING poll
    refreshStatus(status); // PAUSED_PLAYBACK poll -> same ObserveDeviceTransport path as GENA
    assert(cliPauses == 1 && ended == 1);
    resumeState.command('p'); // model LMS acknowledgement
    refreshStatus(status); // TRANSITIONING poll -> ResumeSqueezeBox -> LMS play
    assert(cliPlays == 1);
    std::cout << "PASS: own polling feeds production ObserveDeviceTransport and ResumeSqueezeBox (one LMS pause/play)\n";
}
