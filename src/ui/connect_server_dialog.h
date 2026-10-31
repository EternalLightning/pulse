// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// connect_server_dialog.h - 连接服务器 (pulse_network_design.html §02): one
// address field with a live preview, server suggestions and pin after connecting.
// The same window, in rename mode,
// renames a network pin (one field and a note).
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct ConnectServerSuggestion {
    std::wstring path;  // what the row fills in (\\host or \\host\share)
    std::wstring hint;  // where it came from: 已固定 / 局域网设备 / 最近访问
};

struct ConnectServerPreview {
    bool ok = false;       // 连接 is allowed
    std::wstring text;     // the line under the field (empty: nothing to say)
};

struct ConnectServerDialogSpec {
    bool rename = false;  // rename mode: label, field, note, 取消 / confirm
    std::wstring title;
    std::wstring label;
    std::wstring note;  // rename mode only
    std::wstring initial_text;
    std::wstring confirm_text;
    // Called on every edit; both run on the UI thread and must not block.
    std::function<std::vector<ConnectServerSuggestion>(const std::wstring&)> suggestions;
    std::function<ConnectServerPreview(const std::wstring&)> preview;
    bool pin = true;
};

struct ConnectServerDialogResult {
    bool accepted = false;
    std::wstring text;
    bool pin = false;
};

ConnectServerDialogResult ShowConnectServerDialog(HWND owner, const ConnectServerDialogSpec& spec,
                                                  bool dark, D2D1_COLOR_F accent);

} // namespace pulse::ui
