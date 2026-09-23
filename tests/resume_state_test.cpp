#include "resume_state.h"
#include "stop_debounce.h"
#include <cassert>
#include <iostream>
int main() {
    using Unpause = ResumeState::Unpause;
    for (bool withStop : {false, true}) {
        ResumeState seek;
        seek.command('s'); seek.observe("PLAYING");
        seek.command('p');
        if (withStop) seek.command('q');
        seek.command('s');
        // An old held GET cannot override the new-stream decision.
        assert(seek.command('u', true) == Unpause::NewStream);
        seek.command('p');
        assert(seek.command('u') == Unpause::SameURL);
    }
    ResumeState held;
    held.command('p');
    assert(held.command('u', true) == Unpause::FeedHeldGet);
    held.observe("PLAYING");
    held.observe("PAUSED_PLAYBACK"); held.command('p');
    held.observe("TRANSITIONING");
    assert(held.takeResume(1, 1));
    assert(held.command('u', true) == Unpause::FeedHeldGet); // held GET is actually open
    held.command('p');
    assert(held.command('u') == Unpause::SameURL);
    ResumeState playingSeek;
    playingSeek.command('s'); playingSeek.observe("PLAYING");
    playingSeek.command('q'); playingSeek.command('s');
    assert(playingSeek.command('u') == Unpause::SameURL);
    std::cout << "PASS: paused p/s/u and p/q/s/u choose only new stream; held GET/device play wins over same-URL resume\n";
    ResumeState state;
    state.command('s'); state.observe("PLAYING");
    assert(!state.takeResume(1, 1)); // first GET
    assert(!state.takeResume(1, 1)); // normal second GET
    state.command('p');
    assert(!state.observe("PAUSED_PLAYBACK")); // LMS-origin pause
    state.command('u');
    assert(!state.observe("PAUSED_PLAYBACK")); // cached state after LMS resume
    state.observe("TRANSITIONING");
    assert(!state.takeResume(1, 1));
    state.observe("PLAYING");
    assert(state.observe("PAUSED_PLAYBACK")); // app pause relays exactly once
    assert(!state.observe("PAUSED_PLAYBACK"));
    state.observe("TRANSITIONING");
    assert(!state.takeResume(1, 1)); // LMS p has not arrived yet
    state.command('p');
    assert(!state.takeResume(1, 2)); // old GET never resumes
    assert(state.takeResume(2, 2));
    assert(!state.takeResume(2, 2)); // duplicate GET/event cannot replay command
    state.command('u'); state.observe("PLAYING");
    assert(!state.takeResume(2, 2));
    state.command('q');
    assert(!state.observe("PAUSED_PLAYBACK"));
    state.observe("PLAYING");
    assert(!state.takeResume(2, 2)); // stop is not pause
    StopDebounce stop;
    auto now = StopDebounce::Clock::now();
    unsigned pauses = 0;
    stop.schedule(now); // q
    if (stop.takeDue(now + std::chrono::milliseconds(100))) ++pauses;
    assert(stop.cancel()); // s within 100 ms
    if (stop.takeDue(now + std::chrono::milliseconds(500))) ++pauses;
    assert(pauses == 0);
    stop.schedule(now); // q alone
    if (stop.takeDue(now + std::chrono::milliseconds(399))) ++pauses;
    assert(pauses == 0);
    if (stop.takeDue(now + std::chrono::milliseconds(400))) ++pauses;
    if (stop.takeDue(now + std::chrono::milliseconds(800))) ++pauses;
    assert(pauses == 1);
    std::cout << "PASS: q then s within 100ms sends no Pause; q alone sends one after 400ms\n";
    std::cout << "PASS: resume classification, event ordering, duplicate GETs, LMS unpause, stop\n";
}
