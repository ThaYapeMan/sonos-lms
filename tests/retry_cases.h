static void retryCases() {
    auto reset = [] {
        streamId = 6; completedStream = 5;
        streamStartRetry = RetryBudget{}; retryStream = 0; retryRevision = 0;
        transportIntent = TransportIntent{};
        resumeState = ResumeState{};
        responseOpen = responseEnded = false;
        streamPlays = transportPlays = heldGetInvalidations = 0;
        callOrder.clear(); ourStreamStarted = true; lmsPaused = false;
        testingStreamStart = true; cliResult = true;
        player.property = {"STOPPED", "OK"};
    };
    reset(); playStreamFailures = 1;
    dispatchStreamStart();
    assert(streamPlays == 1 && completedStream == 5);
    dispatchStreamStart();
    assert(streamPlays == 1); // backoff is not a tight loop
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    dispatchStreamStart();
    assert(streamPlays == 2 && completedStream == 6);
    dispatchStreamStart(); assert(streamPlays == 2);
    puts("PASS: failed stream start stays incomplete; one-second retry succeeds and stops retrying");

    reset(); playStreamFailures = 3;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1010));
        dispatchStreamStart();
    }
    assert(streamPlays == 3 && completedStream == 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    dispatchStreamStart(); assert(streamPlays == 3);
    sonos_lms_transport('u'); // explicit command rearms the exhausted setup
    dispatchStreamStart();
    assert(streamPlays == 4 && completedStream == 6);
    dispatchTransportIntent(); assert(streamPlays == 4); // setup already played
    puts("PASS: stream setup gives up after three attempts; new command rearms it without duplicate PlayStream");

    reset(); testingStreamStart = false; completedStream = 6; playStreamFailures = 1;
    sonos_lms_transport('u');
    assert(streamPlays == 1 && transportIntent.pending);
    dispatchTransportIntent(); assert(streamPlays == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    dispatchTransportIntent();
    assert(streamPlays == 2 && !transportIntent.pending);
    puts("PASS: same-URL PlayStream failure retries with backoff and preserves the latest intent");

    reset(); testingStreamStart = false; completedStream = 6; cliPlays = 0;
    resumeState.command('p'); resumeState.stopForPause(6); resumeState.observe("STOPPED");
    player.property.TransportState = "TRANSITIONING";
    cliResult = false;
    ResumeSqueezeBox(6); ResumeSqueezeBox(6);
    assert(cliPlays == 1); // failed CLI does not immediately spam a retry
    assert(resumeState.expireResume(ResumeState::Clock::now() + std::chrono::seconds(5)));
    cliResult = true;
    ResumeSqueezeBox(6);
    assert(cliPlays == 2);
    puts("PASS: failed LMS CLI keeps a lease; expiry permits a new attempt without strm u");
    reset(); testingStreamStart = false; completedStream = 6;
    playStreamFailures = 0; cliResult = true;
}
