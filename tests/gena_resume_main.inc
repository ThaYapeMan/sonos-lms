int main(int argc, char** argv) {
    assert(argc == 3);
    setenv("SONOS_LMS_PAUSE", "stop", 1);
    const std::string order = argv[1];
    const unsigned port = std::stoul(argv[2]);
    upnp::OwnSpeakerControl control([] { return 1450u; }, port, {}, [] { onSonosEvent(nullptr); });
    eventControl = &control;
    assert(control.discover("Study", "127.0.0.1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    resumeState.command('s'); resumeState.observe("PLAYING");
    responseOpen = true;
    sonos_lms_transport('q');
    std::this_thread::sleep_for(std::chrono::milliseconds(410));
    dispatchDeferredStop();
    assert(stopCalls == 1 && resumeState.stoppedForPause(6));
    upnp::TransportInfo info;
    assert(control.readTransportInfo(info) && info.state == "STOPPED");
    bridge::Status status;
    refreshStatus(status);
    assert(cliPlays == 0);
    if (order == "get-first") {
        responseOpen = true; // current-ID GET reaches its production resume hook
        ResumeSqueezeBox(6);
        assert(cliPlays == 0);
    }
    assert(upnp::httpPost({"127.0.0.1", "/test-notify", port}, {}, "").status == 200);
    assert(gEvent.exchange(false));
    refreshStatus(status); // same onSonosEvent -> gEvent -> refreshStatus path
    responseOpen = true;
    ResumeSqueezeBox(6); ResumeSqueezeBox(6); refreshStatus(status);
    assert(cliPlays == 1 && resumeLogs == 1 && cliPauses == 0);
    sonos_lms_transport('u');
    assert(responseOpen && !responseEnded && streamPlays == 0 && transportPlays == 0);
    std::cout << "PASS: GENA " << order << ": q-Stop, current-stream GET, one Device-initiated resume and one LMS play\n";
    eventControl = nullptr;
}
