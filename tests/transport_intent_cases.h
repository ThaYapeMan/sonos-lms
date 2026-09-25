static void transportIntentCases() {
    auto reset = [] {
        resumeState = ResumeState{};
        transportIntent = TransportIntent{};
        deferredStop = StopDebounce{};
        streamId = completedStream = 6;
        ourStreamStarted = true;
        lmsPaused = false;
        responseOpen = true; responseEnded = false;
        pauseCalls = stopCalls = responseEnds = 0;
        streamPlays = transportPlays = heldGetInvalidations = 0;
        callOrder.clear();
        resumeState.command('s'); resumeState.observe("PLAYING");
        player.property = {"PLAYING", "OK"};
    };
    for (const char* commands : {"p", "u", "pu"}) {
        reset();
        // Use a different owner thread: try_lock on one's own mutex is invalid.
        std::atomic<bool> acquired{false}, release{false};
        std::thread owner([&] {
            std::lock_guard<std::mutex> lock(transportMutex);
            acquired = true;
            while (!release) std::this_thread::yield();
        });
        while (!acquired) std::this_thread::yield();
        if (std::string(commands) != "p") responseOpen = false;
        for (char command : std::string(commands)) sonos_lms_transport(command);
        dispatchTransportIntent();
        assert(transportIntent.pending && transportIntent.deferred);
        assert(pauseCalls == 0 && stopCalls == 0 && streamPlays == 0);
        release = true; owner.join();
        dispatchTransportIntent();
        assert(!transportIntent.pending);
        if (std::string(commands) == "p") {
            assert(responseEnds == 1);
            assert(pauseCalls + stopCalls == 1 && streamPlays == 0);
        } else assert(pauseCalls + stopCalls == 0 && streamPlays == 1);
        dispatchTransportIntent(); // no duplicate application
        printf("PASS: busy transport %s converges once on latest intent\n", commands);
    }
    reset();
    completedStream = 5;
    sonos_lms_transport('p');
    dispatchTransportIntent();
    assert(transportIntent.pending && pauseCalls + stopCalls == 0);
    completedStream = 6;
    dispatchTransportIntent();
    assert(pauseCalls + stopCalls == 1 && !transportIntent.pending);
    puts("PASS: restart window retains pause until stream setup completes");
    reset();
}
