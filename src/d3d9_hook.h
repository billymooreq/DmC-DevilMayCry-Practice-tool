#pragma once

// IDirect3DDevice9 Present/EndScene/Reset hooks (vtable taken from a dummy device) + ImGui lifecycle.
#include <string>

namespace d3d9hook {
void Install();  // blocking, called from the init thread
// Saves the next presented frame (incl. our overlay) as a 24-bit BMP. Returns after it is written or
// after timeoutMs; false on failure/timeout.
bool Screenshot(const std::wstring& path, int timeoutMs = 5000);
void RequestTrace();  // Dev: log the render targets, depth surfaces, clears and draws of the next frame
}
