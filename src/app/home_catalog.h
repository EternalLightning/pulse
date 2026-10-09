#pragma once
#include "../ui/ui_renderer.h"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace pulse::model { class PlacesCatalog; }

namespace pulse::app {
class HomeCatalog {
public:
    void Refresh();
    void UpdateDriveLabel(const std::wstring& root, const std::wstring& label);
    bool ConsumeUpdate();
    bool Loading() const;
    void Fill(ui::PaneViewModel& pane, const model::PlacesCatalog* places = nullptr) const;
private:
    struct State {
        std::mutex mutex;
        std::vector<ui::HomeCardView> cards;
        std::unordered_map<std::wstring, std::wstring> label_updates;
        bool loading = false;
        bool updated = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};
}
