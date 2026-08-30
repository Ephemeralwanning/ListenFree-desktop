#include "media/playback_state_machine.h"

namespace listenfree::media {

bool PlaybackStateMachine::transition(domain::PlaybackState next) noexcept {
    const auto current = state_;
    const bool allowed =
        (current == domain::PlaybackState::Idle && (next == domain::PlaybackState::Loading || next == domain::PlaybackState::Stopped)) ||
        (current == domain::PlaybackState::Loading && (next == domain::PlaybackState::Playing || next == domain::PlaybackState::Error || next == domain::PlaybackState::Stopped)) ||
        (current == domain::PlaybackState::Playing && (next == domain::PlaybackState::Paused || next == domain::PlaybackState::Buffering || next == domain::PlaybackState::Stopped || next == domain::PlaybackState::Error)) ||
        (current == domain::PlaybackState::Paused && (next == domain::PlaybackState::Playing || next == domain::PlaybackState::Stopped || next == domain::PlaybackState::Loading)) ||
        (current == domain::PlaybackState::Buffering && (next == domain::PlaybackState::Playing || next == domain::PlaybackState::Paused || next == domain::PlaybackState::Error || next == domain::PlaybackState::Stopped)) ||
        (current == domain::PlaybackState::Stopped && (next == domain::PlaybackState::Loading || next == domain::PlaybackState::Idle)) ||
        (current == domain::PlaybackState::Error && (next == domain::PlaybackState::Loading || next == domain::PlaybackState::Idle));
    if (!allowed) return false;
    state_ = next;
    return true;
}

} // namespace listenfree::media
