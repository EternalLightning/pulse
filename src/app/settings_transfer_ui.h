#pragma once
#include <string>

namespace pulse { struct AppState; }
namespace pulse::app { struct SettingsTask; struct SettingsTaskResult; }

namespace pulse {
struct SettingsTransferView {
    bool busy = false, pending = false, has_backup = false;
    std::wstring backup_time;
};
SettingsTransferView GetSettingsTransferView();
void SettingsTransferAction(AppState& s, int action);
bool RunSettingsTransferTask(const app::SettingsTask& task, std::wstring& error);
void FinishSettingsTransfer(AppState& s, const app::SettingsTaskResult& result);
bool WaitForSettingsRestart(unsigned long process_id);
void ApplyStagedSettingsAtStartup();
void ShowSettingsTransferStartupToast(AppState& s);
}
