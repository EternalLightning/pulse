#pragma once
#include "../fs/cloud_pin.h"
#include "../ui/fluent_menu.h"

namespace pulse {
struct AppState;
namespace app {
struct Tab;
struct CloudPinCompletion {
    std::vector<std::wstring> paths;
    fs::CloudPinResult result;
};
struct CloudPreviewIntent {
    uint64_t tab = 0;
    uint64_t generation = 0;
    uint64_t selection = 0;
    std::wstring folder;
    std::wstring path;
};
}
bool HandleCloudCommand(AppState& s, int command);
void AppendCloudCommands(const AppState& s, const app::Tab& tab, std::vector<ui::FluentMenuItem>& items);
void HandleCloudDownloadResults(AppState& s, WPARAM identity);
void HandleCloudPinResults(AppState& s, WPARAM identity);
bool TickCloudStatus(AppState& s);
void CloseCloudTasks(AppState& s);
} // namespace pulse
