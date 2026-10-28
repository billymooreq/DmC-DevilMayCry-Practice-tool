#pragma once
#include <atomic>

// Shared runtime state.
namespace state {
inline std::atomic<bool> menuOpen{false};

}  // namespace state
