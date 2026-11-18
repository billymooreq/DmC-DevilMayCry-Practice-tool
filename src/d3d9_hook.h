#pragma once

// IDirect3DDevice9 Present/EndScene/Reset hooks (vtable taken from a dummy device) + ImGui lifecycle.
#include <string>

namespace d3d9hook {
void Install();  // blocking, called from the init thread
// Saves the next presented frame (incl. our overlay) as a 24-bit BMP. Returns after it is written or
// after timeoutMs; false on failure/timeout.
bool Screenshot(const std::wstring& path, int timeoutMs = 5000);
void RequestTrace();
// IDirect3DDevice9 vtable entry i (read from the dummy device at Install; null before).
void* DeviceVtableEntry(int i);
// A texture for ImGui::Image from 32-bit BGRA pixels (managed pool: survives a device reset). Render thread.
void* CreateTextureBGRA(const void* pixels, int w, int h);
void ReleaseTexture(void* texture);  // one from CreateTextureBGRA (render thread)  // Dev: log the render targets, depth surfaces, clears and draws of the next frame
}
