// Production transport functions run against the fixture's device/LMS boundary.
static void stopPauseCases() {
    auto pause = [] {
        resumeState = ResumeState{};
        deferredStop = StopDebounce{};
        responseOpen = true; responseEnded = false;
        cliPlays = cliPauses = pauseCalls = stopCalls = responseEnds = 0;
        streamPlays = transportPlays = heldGetInvalidations = 0;
        callOrder.clear();
        resumeState.command('s'); resumeState.observe("PLAYING");
        sonos_lms_transport('p');
        assert(stopCalls == 1 && pauseCalls == 0 && responseEnds == 1);
        assert(resumeState.stoppedForPause(6));
        player.property = {"STOPPED", "OK"};
        ObserveDeviceTransport("STOPPED");
        ResumeSqueezeBox(6);
        assert(cliPlays == 0 && cliPauses == 0 && streamPlays == 0 && transportPlays == 0);
    };
    for (const char* state : {"TRANSITIONING", "PLAYING"}) {
        pause();
        responseOpen = true; // fresh device GET, waiting for LMS PCM
        player.property.state = state;
        ResumeSqueezeBox(6); ResumeSqueezeBox(6);
        assert(cliPlays == 1);
        sonos_lms_transport('u');
        assert(responseOpen && !lmsPaused && !responseEnded);
        assert(streamPlays == 0 && transportPlays == 0 && heldGetInvalidations == 0);
        ObserveDeviceTransport("PLAYING");
        assert(!resumeState.stoppedForPause(6));
    }
    puts("PASS: pause sends Stop after EOF; STOPPED is ignored; fresh GET after STOPPED resumes LMS once without a second device command");
    pause();
    sonos_lms_transport('u'); // LMS resume without an open GET
    assert(streamPlays == 1 && transportPlays == 0);
    ObserveDeviceTransport("PLAYING");
    assert(!resumeState.stoppedForPause(6));
    puts("PASS: LMS resume without GET uses PlayStream(same URL)");
    pause();
    new_squeezebox_stream_id();
    assert(!resumeState.stoppedForPause(6) && !resumeState.stoppedForPause(7));
    assert(!resumeState.takeResume(7, 7));
    streamId = 6;
    for (char command : {'s', 'q'}) {
        pause(); sonos_lms_transport(command);
        assert(!resumeState.stoppedForPause(6));
        assert(stopCalls == 1);
    }
    resumeState = ResumeState{};
    resumeState.command('s'); resumeState.observe("PLAYING");
    cliPauses = 0;
    ObserveDeviceTransport("PAUSED_PLAYBACK");
    assert(cliPauses == 1); // the ensuing LMS p must use Stop too
    responseEnds = stopCalls = 0; responseOpen = true;
    sonos_lms_transport('p');
    assert(stopCalls == 1 && responseEnds == 1);
    puts("PASS: device pause relays LMS pause, whose strm p sends Stop");
}

static void deferredStopCases() {
    auto setup = [] {
        resumeState = ResumeState{};
        deferredStop = StopDebounce{};
        responseOpen = true; responseEnded = false;
        cliPlays = cliPauses = pauseCalls = stopCalls = responseEnds = 0;
        streamPlays = transportPlays = heldGetInvalidations = 0;
        callOrder.clear();
        resumeState.command('s'); resumeState.observe("PLAYING");
        sonos_lms_transport('q');
        dispatchDeferredStop();
        assert(stopCalls == 0 && pauseCalls == 0 && responseEnds == 1);
    };
    setup();
    sonos_lms_transport('s');
    std::this_thread::sleep_for(std::chrono::milliseconds(410));
    dispatchDeferredStop();
    assert(stopCalls == 0 && pauseCalls == 0);
    puts("PASS: q then s cancels the deferred transport command");
    for (const char* next : {"TRANSITIONING", "PLAYING"}) {
        setup();
        std::this_thread::sleep_for(std::chrono::milliseconds(410));
        dispatchDeferredStop(); dispatchDeferredStop();
        if (pauseMode() == PauseMode::Pause) {
            assert(pauseCalls == 1 && stopCalls == 0);
            assert(!resumeState.stoppedForPause(6));
            continue;
        }
        assert(stopCalls == 1 && pauseCalls == 0 && resumeState.stoppedForPause(6));
        player.property = {"STOPPED", "OK"};
        bridge::Status status;
        refreshStatus(status); refreshStatus(status);
        assert(cliPlays == 0 && cliPauses == 0 && streamPlays == 0 && transportPlays == 0);
        responseOpen = true; // fresh GET
        player.property.state = next;
        ResumeSqueezeBox(6); ResumeSqueezeBox(6); refreshStatus(status);
        assert(cliPlays == 1 && cliPauses == 0);
        sonos_lms_transport('u');
        assert(responseOpen && !lmsPaused && !responseEnded);
        assert(streamPlays == 0 && transportPlays == 0 && heldGetInvalidations == 0);
    }
    puts("PASS: q after 400 ms uses configured Stop/Pause; q-Stop ignores STOPPED and fresh GET sends LMS play once");
}
