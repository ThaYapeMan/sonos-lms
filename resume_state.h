#ifndef RESUME_STATE_H
#define RESUME_STATE_H

#include <string>

// Caller serializes access. HTTP requests never change LMS transport intent.
class ResumeState {
public:
    enum class Unpause { None, NewStream, FeedHeldGet, SameURL };

    Unpause command(char command, bool responseOpen = false) {
        Unpause action = Unpause::None;
        if (command == 'u') {
            // Decide before clearing the device-resume request. Exactly one
            // action wins, even when a paused seek also has an old held GET.
            action = newStreamPending ? Unpause::NewStream
                : (responseOpen || requested) ? Unpause::FeedHeldGet
                : Unpause::SameURL;
            newStreamPending = pausedBeforeStop = false;
        } else if (command == 's') {
            newStreamPending = newStreamPending || paused || pausedBeforeStop;
            pausedBeforeStop = false;
        } else if (command == 'q') {
            // A debounced q before s must not erase the preceding pause.
            pausedBeforeStop = paused || pausedBeforeStop;
            newStreamPending = false;
        }
        if (command == 'p') { paused = true; awaitingPlay = false; expectedPause = true; }
        else if (command == 'u' || command == 's' || command == 'q') {
            paused = false;
            awaitingPlay = command == 'u' || command == 's';
            expectedPause = command == 'q';
            sawPause = transitioning = requested = false;
        }
        return action;
    }

    // Returns true only for a device pause not already commanded by LMS.
    bool observe(const std::string& state) {
        if (state == "PAUSED_PLAYBACK") {
            if (awaitingPlay) return false; // cached pause after an LMS unpause
            bool relay = !sawPause && !paused && !expectedPause;
            sawPause = true;
            transitioning = false;
            return relay;
        }
        if (state == "PLAYING" || state == "TRANSITIONING") {
            transitioning = sawPause;
            if (state == "PLAYING") awaitingPlay = false;
        }
        else
            transitioning = false;
        return false;
    }

    bool takeResume(unsigned requestedId, unsigned currentId) {
        if (requestedId != currentId || !paused || !sawPause || !transitioning || requested)
            return false;
        requested = true;
        return true;
    }

    void retryResume() { requested = false; }

private:
    bool newStreamPending = false;
    bool pausedBeforeStop = false;
    bool awaitingPlay = false;
    bool expectedPause = false;
    bool paused = false; // strm p seen, no u/s/q since
    bool sawPause = false;
    bool transitioning = false;
    bool requested = false;
};
#endif
