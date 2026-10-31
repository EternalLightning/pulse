// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ui/ui_renderer.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulse::model { class PlacesCatalog; }

namespace pulse::app {
struct SidebarModel;
class HomeCatalog {
public:
    void Refresh();
    void UpdateDriveLabel(const std::wstring& root, const std::wstring& label);
    bool ConsumeUpdate();
    bool Loading() const;
    void FillDriveLabels(SidebarModel& sidebar) const;
    void Fill(ui::PaneViewModel& pane, const model::PlacesCatalog* places = nullptr) const;
private:
    struct State {
        std::mutex mutex;
        std::vector<ui::HomeCardView> cards;
        std::unordered_map<std::wstring, std::pair<uint64_t, uint64_t>> drive_capacity;
        std::unordered_map<std::wstring, std::wstring> label_updates;
        uint64_t generation = 0;
        uint64_t published_generation = 0;
        bool loading = false;
        bool updated = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};
}
