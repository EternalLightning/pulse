#pragma once
#include "../model/places.h"

namespace pulse::app {
using model::TagId;
using model::TagSelectionState;
using model::Workspace;
using model::ColorTag;
using model::TagAdsRecord;
using model::NetworkPlace;
using model::PlaceItemKind;
using model::StarredItem;
using model::kDefaultBadgeRgb;
using model::QuickAccessBadge;
using model::RecentItem;
using model::RecentFilter;
using model::TagAdsUpdate;
using model::PlacesCatalog;
using model::MakeStarredPath;
using model::MakeRecentPath;
using model::MakeRecyclePath;
using model::MakeHomePath;
using model::MakeTagPath;
using model::MakeSearchPath;
using model::MakeWorkspacePath;
using model::MakeSettingsPath;
using model::ParsePulsePath;
using model::WriteTagAdsV2;
using model::ReadTagAds;
using model::ReadTagAdsV2;
} // namespace pulse::app
