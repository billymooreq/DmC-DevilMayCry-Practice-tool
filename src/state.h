#pragma once
#include <atomic>

// Shared runtime state.
namespace state {
inline std::atomic<bool> menuOpen{false};

// Freecam (camera.cpp): while set, the game gets no keyboard / mouse / gamepad input (Esc and the gamepad's
// Start still pass: the pause menu works); the mouse movement it would have seen is collected below instead.
inline std::atomic<bool> freecamMute{false};

// Autorun tests only (#23, no gamepad on the test machine): a pretend Xbox pad 0 for the game's XInputGetState.
// padFakeButtons: XINPUT wButtons in the low 16 bits, bit 16 = left trigger, bit 17 = right trigger.
inline std::atomic<bool> padFake{false};
inline std::atomic<unsigned> padFakeButtons{0};
inline std::atomic<int> padFakeLX{0}, padFakeLY{0};
inline std::atomic<unsigned> xinputCalls[4] = {0, 0, 0, 0};  // XInputGetState calls per pad index (padfake log)
// Mouse movement while muted, per source the game reads (DirectInput state / buffered data, Raw Input).
// Freecam takes them with exchange(0). Counters: how often each source was seen (which one the game uses).
inline std::atomic<int> fcStateX{0}, fcStateY{0}, fcStateZ{0};
inline std::atomic<int> fcDataX{0}, fcDataY{0}, fcDataZ{0};
inline std::atomic<int> fcRawX{0}, fcRawY{0}, fcRawZ{0};
inline std::atomic<unsigned> fcSeenState{0}, fcSeenData{0}, fcSeenRaw{0};

}  // namespace state
