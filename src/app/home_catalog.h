#pragma once
#include "../ui/ui_renderer.h"
#include <memory>
#include <mutex>

namespace pulse::app {
class HomeCatalog {
public:
    void Refresh();
    bool ConsumeUpdate();
    bool Loading() const;
    void Fill(ui::PaneViewModel& pane, const PlacesCatalog* places = nullptr) const;
private:
    struct State {
        std::mutex mutex;
        std::vector<ui::HomeCardView> cards;
        bool loading = false;
        bool updated = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};
}
