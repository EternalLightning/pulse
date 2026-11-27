// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ui/ui_renderer.h"
#include "../model/home_quick_access.h"
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulse::model { class PlacesCatalog; }

namespace pulse::app {
struct SidebarModel;
class HomeCatalog {
public:
    void Refresh(const model::HomeQuickAccessPrefs& prefs = {});
    void SetQuickAccessPrefs(const model::HomeQuickAccessPrefs& prefs);
    void UpdateDriveLabel(const std::wstring& root, const std::wstring& label);
    bool ConsumeUpdate();
    bool Loading() const;
    void FillDriveLabels(SidebarModel& sidebar) const;
    void Fill(ui::PaneViewModel& pane, const model::PlacesCatalog* places = nullptr) const;
private:
    struct State {
        std::mutex mutex;
        std::vector<ui::HomeCardView> cards;
        std::array<ui::HomeCardView, model::kHomeQuickAccessItemCount> quick_access_cards;
        std::vector<ui::HomeCardView> visible_quick_access_cards;
        std::unordered_map<std::wstring, std::pair<uint64_t, uint64_t>> drive_capacity;
        DWORD drives_without_media = 0;
        std::unordered_map<std::wstring, std::wstring> label_updates;
        uint64_t generation = 0;
        model::HomeQuickAccessPrefs quick_access;
        bool loading = false;
        bool updated = false;
        void UpdateQuickAccessPrefs(const model::HomeQuickAccessPrefs& prefs);
        void RebuildQuickAccess();
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};
}
