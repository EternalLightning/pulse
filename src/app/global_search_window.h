// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include "../ui/window_material.h"

namespace pulse::index { class IndexClient; class NetworkAgentClient; }

namespace pulse {
class GlobalSearchWindow {
public:
    GlobalSearchWindow();
    ~GlobalSearchWindow();
    GlobalSearchWindow(const GlobalSearchWindow&) = delete;
    GlobalSearchWindow& operator=(const GlobalSearchWindow&) = delete;
    bool Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin = true);
    // Bind before Show; the owner must outlive this window and retain Start/Stop ownership.
    void BindFilenameClients(index::IndexClient& filenames, index::NetworkAgentClient& network, uint32_t& next_request);
    // Invoked on the UI thread; callback targets must outlive this window.
    void SetOpenCallback(std::function<void(const std::wstring&, bool)> callback);
    void SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent);
    void Hide();
    void Shutdown();
    bool Visible() const;
private:
    friend struct GlobalSearchWindowTestPeer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
