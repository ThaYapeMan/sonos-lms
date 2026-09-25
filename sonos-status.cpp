// sonos-status.cpp -- poll Sonos transport/track state and report changes
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the sonos-lms project. Licensed under
// the GNU General Public License, version 3 or (at your option) any later
// version, matching the rest of this project. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#include "sonos-status.h"

#include <cstdio>
#include <cstdlib>

using namespace NSROOT;

namespace {
constexpr char kNoPosition[] = "-:--:--";
}

bool Status::Snapshot::operator==(const Snapshot& other) const
{
    return title == other.title
        && album == other.album
        && artist == other.artist
        && transportStatus == other.transportStatus
        && transportState == other.transportState
        && relativeTime == other.relativeTime
        && trackDuration == other.trackDuration
        && volume == other.volume;
}

Status::Status(PlayerPtr player)
    : m_player(player)
{
    if (m_player) {
        m_zoneUuid = m_player->GetZone()->GetCoordinator()->GetUUID();
        m_zoneName = m_player->GetZone()->GetZoneName();
    }
    m_current.relativeTime = kNoPosition;
    m_current.trackDuration = kNoPosition;
}

Status::Snapshot Status::poll() const
{
    Snapshot s;
    s.relativeTime = kNoPosition;
    s.trackDuration = kNoPosition;

    if (!m_player || m_player->TransportPropertyEmpty())
        return s;

    if (!m_player->GetVolume(m_zoneUuid, &s.volume))
        s.volume = 0;

    SONOS::ElementList position;
    if (m_player->GetPositionInfo(position))
        s.relativeTime = position.GetValue("RelTime");

    SONOS::AVTProperty transport = m_player->GetTransportProperty();
    if (transport.CurrentTrackMetaData) {
        s.title = transport.CurrentTrackMetaData->GetValue("dc:title");
        s.album = transport.CurrentTrackMetaData->GetValue("upnp:album");
        s.artist = transport.CurrentTrackMetaData->GetValue("dc:creator");
    }
    s.transportStatus = transport.TransportStatus;
    s.transportState = transport.TransportState;
    s.trackDuration = transport.CurrentTrackDuration;
    return s;
}

void Status::update()
{
    m_current = poll();
}

bool Status::changed()
{
    if (m_haveReported && m_current == m_lastReported)
        return false;
    m_lastReported = m_current;
    m_haveReported = true;
    return true;
}

size_t Status::hash()
{
    // No longer used internally (changed() compares snapshots directly);
    // kept so callers built against the earlier interface still link.
    size_t h = std::hash<std::string>{}(m_current.title);
    h ^= std::hash<std::string>{}(m_current.album) << 1;
    h ^= std::hash<std::string>{}(m_current.artist) << 2;
    return h;
}

void Status::print()
{
    const std::string border(97, '-');
    int nameField = static_cast<int>(border.length()) - static_cast<int>(m_zoneName.length()) - 5;
    if (nameField < 0)
        nameField = 0;
    printf("\n+%s %s ---+\n", border.substr(0, static_cast<size_t>(nameField)).c_str(), m_zoneName.c_str());
    printf("| Title  %-84s |\n", m_current.title.c_str());
    if (!m_current.album.empty())
        printf("| Album  %-84s |\n", m_current.album.c_str());
    if (!m_current.artist.empty())
        printf("| Artist %-84s |\n", m_current.artist.c_str());
    printf("+%s+\n", border.c_str());
    printf("| %-26s | %-24s | vol %3u / 100 | %8s / %8s |\n",
        m_current.transportStatus.c_str(),
        m_current.transportState.c_str(),
        static_cast<unsigned>(m_current.volume),
        m_current.relativeTime.c_str(),
        m_current.trackDuration.c_str());
    printf("+%s+\n", border.c_str());
}

void Status::get_mac(uint8_t* mac) const
{
    // Sonos zone UUIDs take the form "RINCON_" followed by the 12 hex
    // digits of the player's MAC address (e.g. "RINCON_5CAAFDXXXXXX01400").
    static constexpr size_t kPrefixLen = 7;   // strlen("RINCON_")
    static constexpr size_t kMacHexLen = 12;  // 6 bytes, 2 hex chars each
    if (m_zoneUuid.length() < kPrefixLen + kMacHexLen)
        return;
    for (size_t i = 0; i < 6; ++i) {
        char byteHex[3] = {
            m_zoneUuid[kPrefixLen + 2 * i],
            m_zoneUuid[kPrefixLen + 2 * i + 1],
            '\0',
        };
        mac[i] = static_cast<uint8_t>(std::strtoul(byteHex, nullptr, 16));
    }
}
