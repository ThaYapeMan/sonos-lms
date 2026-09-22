// sonos-status.h -- poll Sonos transport/track state and report changes
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-squeezebox project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#ifndef SONOS_STATUS_H
#define SONOS_STATUS_H

#include <cstdint>
#include <string>

#include <sonosplayer.h>
#include <sonossystem.h>

namespace NSROOT {

// Polls a Sonos zone player's transport and current-track metadata, and
// reports whether anything worth logging has changed since the last poll.
class Status {
public:
    explicit Status(PlayerPtr player);
    ~Status() = default;

    // Re-reads transport/track state from the player. Call before changed()
    // or print() reflect anything new.
    void update();

    // True if the polled state differs from what was seen on the previous
    // update(). Also latches the new state as the comparison baseline for
    // next time.
    bool changed();

    // Dumps the current state as a small boxed table on stdout.
    void print();

    // Retained for interface compatibility with earlier revisions of this
    // class; changed() no longer needs a separate hash step.
    size_t hash();

    // Extracts the player's 6-byte MAC address from its Sonos zone UUID
    // (format "RINCON_<12 hex chars><...>") into a caller-supplied buffer.
    void get_mac(uint8_t* mac) const;

    const std::string& getTransportState() const { return m_current.transportState; }

private:
    struct Snapshot {
        std::string title;
        std::string album;
        std::string artist;
        std::string transportStatus;
        std::string transportState;
        std::string relativeTime;
        std::string trackDuration;
        uint8_t volume = 0;

        bool operator==(const Snapshot& other) const;
        bool operator!=(const Snapshot& other) const { return !(*this == other); }
    };

    PlayerPtr m_player;
    std::string m_zoneUuid;
    std::string m_zoneName;

    Snapshot m_current;
    Snapshot m_lastReported;
    bool m_haveReported = false;

    Snapshot poll() const;
};

}  // namespace NSROOT

#endif  // SONOS_STATUS_H
