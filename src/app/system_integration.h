#pragma once
#include "default_file_manager.h"
#include <memory>

namespace pulse { struct AppState; }
namespace pulse::app {
inline constexpr UINT kSystemIntegrationMessage = WM_APP + 68;
inline constexpr UINT kShellSelectionMessage = WM_APP + 69;
inline constexpr UINT kExplorerTakeoverMessage = WM_APP + 70;
std::wstring SystemExecutablePath();
class SystemIntegration {
public:
    struct Shared;
    struct UiState;
    SystemIntegration();
    ~SystemIntegration();
    void Start(AppState& state);
    // Runs the same task/result mailbox in an isolated fixture, without COM or HKCU.
    bool StartForTesting(AppState& state, DefaultManagerRegistry registry);
    void Toggle(AppState& state, int control);
    void Publish(AppState& state);
    bool Tick(AppState& state);
    bool pending() const;
    const std::wstring& error() const;
    void Stop(); // Invoked by the existing exit worker before destroying HWND.
private:
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<UiState> ui_;
};
} // namespace pulse::app
