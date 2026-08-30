#pragma once

#include "domain/domain.h"

namespace listenfree::media {

class PlaybackStateMachine final {
public:
    [[nodiscard]] domain::PlaybackState state() const noexcept { return state_; }
    bool transition(domain::PlaybackState next) noexcept;
private:
    domain::PlaybackState state_{domain::PlaybackState::Idle};
};

} // namespace listenfree::media
