// Included after the existing fixture and extracted production functions.
static void feedRestartCases() {
    auto beginResume = [] {
        paused("OK");
        responseOpen = true; // Sonos opens its own GET while paused
        player.property.TransportState = "TRANSITIONING";
        ResumeSqueezeBox(6);
        sonos_lms_transport('u');
        assert(responseOpen && !lmsPaused && !responseEnded);
        assert(cliPlays == 1 && streamPlays == 0 && transportPlays == 0);
        assert(heldGetInvalidations == 0 && framesResumeMarks == 0);
        assert(feedRestartWatch.active());
        assert(feedResumeMarks == 1);
    };
    auto stopped = [] {
        // Reference order: GET -> first audio -> HEAD -> client closes ->
        // STOPPED. The HTTP counterpart exercises the actual HEAD/FLAC bytes.
        assert(responseOpen);
        responseOpen = false; // client close, never end_squeezebox_response()
        player.property = {"STOPPED", "OK"};
        ObserveDeviceTransport("STOPPED");
    };
    beginResume();
    stopped();
    dispatchFeedRestart();
    assert(streamPlays == 0); // 200 ms grace period
    {
        FeedRestartWorker worker(dispatchFeedRestart);
        waitFor([] { return streamPlays.load() == 1; });
    } // joins, including the rest of PlaySqueezeBoxLocked
    assert(!feedRestartWatch.active() && !responseEnded);
    ObserveDeviceTransport("STOPPED");
    dispatchFeedRestart();
    assert(streamPlays == 1 && heldGetInvalidations == 0 && transportPlays == 0);
    puts("PASS: fed resume GET -> HEAD/client close -> STOPPED -> one delayed same-URL restart; repeated STOPPED does nothing");

    beginResume();
    player.property.TransportState = "PLAYING";
    ObserveDeviceTransport("PLAYING");
    std::this_thread::sleep_for(std::chrono::milliseconds(2050));
    ObserveDeviceTransport("PLAYING");
    assert(!feedRestartWatch.active());
    stopped(); dispatchFeedRestart();
    assert(streamPlays == 0);
    puts("PASS: PLAYING sustained for two seconds disarms without a restart");

    for (char command : std::string("pqs")) {
        beginResume(); stopped();
        // The existing pause fake expects an open response and one Pause.
        responseOpen = true; responseEnded = false; responseEnds = pauseCalls = 0;
        sonos_lms_transport(command);
        assert(!feedRestartWatch.active());
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        dispatchFeedRestart();
        assert(streamPlays == 0);
        printf("PASS: strm %c cancels a pending feed-restart\n", command);
    }
    beginResume(); stopped();
    streamId = 7;
    dispatchFeedRestart();
    assert(!feedRestartWatch.active() && streamPlays == 0);
    streamId = 6;
    puts("PASS: stream change cancels feed-restart");

    beginResume(); stopped();
    ObserveDeviceTransport("PAUSED_PLAYBACK");
    assert(!feedRestartWatch.active());
    dispatchFeedRestart(); assert(streamPlays == 0);
    puts("PASS: a new device pause cancels feed-restart during its delay");

    beginResume(); stopped();
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    {
        std::lock_guard<std::mutex> busy(transportMutex);
        auto before = std::chrono::steady_clock::now();
        dispatchFeedRestart(); // try-lock: cannot wait on this test thread
        assert(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
        assert(streamPlays == 0 && feedRestartWatch.active());
    }
    dispatchFeedRestart();
    assert(streamPlays == 1 && !feedRestartWatch.active());
    puts("PASS: busy transport keeps the restart pending and retries successfully");

    beginResume(); stopped();
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    {
        std::lock_guard<std::mutex> busy(transportMutex);
        dispatchFeedRestart();
        cancelFeedRestart("LMS strm p");
    }
    dispatchFeedRestart(); assert(streamPlays == 0);
    puts("PASS: cancellation while waiting for transport prevents restart");

    beginResume();
    feedRestartWatch.arm(6, FeedRestartWatch::Clock::now() - std::chrono::seconds(6));
    dispatchFeedRestart();
    assert(!feedRestartWatch.active() && streamPlays == 0);
    puts("PASS: watch deadline expires even without new transport events");

    paused("OK"); // no open GET: original SameURL behavior
    player.property.TransportState = "TRANSITIONING";
    ResumeSqueezeBox(6);
    sonos_lms_transport('u');
    assert(streamPlays == 1 && heldGetInvalidations == 1 && !feedRestartWatch.active());
    puts("PASS: feed-restart without a held GET retains the existing SameURL path");
}
