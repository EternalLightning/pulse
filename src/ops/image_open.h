// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <string>
#include <memory>
#include <windows.h>

namespace pulse::ops {

class ImageOpenSession {
public:
    ImageOpenSession();
    ~ImageOpenSession();
    ImageOpenSession(const ImageOpenSession&) = delete;
    ImageOpenSession& operator=(const ImageOpenSession&) = delete;
    bool ViewerRunning() noexcept;

private:
    friend bool TryOpenImageWithNeighbors(HWND, const std::wstring&,
        std::shared_ptr<void>, ImageOpenSession*) noexcept;
    struct State;
    std::unique_ptr<State> state_;
};

// Runs on the open worker's COM apartment. Returns false when the path is not
// an image or Shell folder activation is unavailable; the caller uses ShellExecute.
bool TryOpenImageWithNeighbors(HWND owner, const std::wstring& path,
                               std::shared_ptr<void> lifetime = {},
                               ImageOpenSession* session = nullptr) noexcept;

} // namespace pulse::ops
