#ifndef TRANSPORT_INTENT_H
#define TRANSPORT_INTENT_H
#include "resume_state.h"
#include <cstdint>

// Serialized by intentMutex. One desired state, never a queue of old commands.
struct TransportIntent {
    uint64_t revision = 0;
    unsigned stream = 0;
    char command = 0;
    ResumeState::Unpause unpause = ResumeState::Unpause::None;
    bool pending = false;
    bool deferred = false;
};
#endif
