// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "default_file_manager.h"
#include "external_open.h"
#include <functional>
#include <memory>

namespace pulse { struct AppState; }
namespace pulse::app {
inline constexpr UINT kSystemIntegrationMessage = WM_APP + 68;
inline constexpr UINT kShellSelectionMessage = WM_APP + 69;
inline constexpr UINT kExplorerTakeoverMessage = WM_APP + 70;
std::wstring SystemExecutablePath();
struct ExplorerTakeoverRequest;
struct SystemTakeoverTestCallbacks {
    std::function<bool(uint64_t)> active;
    std::function<void(uint64_t, bool)> acknowledge;
    std::function<std::vector<ExplorerTakeoverRequest>()> drain;
    std::function<bool(const ExplorerTakeoverRequest&)> route;
};
class SystemIntegration {
public:
    struct Shared;
    struct UiState;
    SystemIntegration();
    ~SystemIntegration();
    void Start(AppState& state);
    // Runs the same task/result mailbox in an isolated fixture, without COM or HKCU.
    bool StartForTesting(AppState& state, DefaultManagerRegistry registry,
                         SystemTakeoverTestCallbacks takeover = {});
    void Toggle(AppState& state, int control);
    void Publish(AppState& state);
    bool IsExternalOpenActive(uint64_t takeover_token) const;
    void CompleteExternalOpen(AppState& owner, AppState* destination,
                              const ExternalOpenResult& request, bool opened);
    bool Tick(AppState& state);
    bool pending() const;
    const std::wstring& error() const;
    void Stop(); // Invoked by the existing exit worker before destroying HWND.
private:
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<UiState> ui_;
};
} // namespace pulse::app
