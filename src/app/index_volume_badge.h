#pragma once
#include "../index/index_config.h"
#include "../ui/fluent_components.h"

namespace pulse::app {

inline ui::fluent::BadgeKind IndexVolumeBadgeKind(const index::VolumeInfo& volume) {
    using K = ui::fluent::BadgeKind;
    if (volume.failed) return K::Danger;
    if (!volume.online) return K::Neutral;
    if (!volume.supported) return K::Warning;
    if (volume.progress == 100) return K::Success;
    return volume.enabled ? K::Accent : K::Neutral;
}

} // namespace pulse::app
