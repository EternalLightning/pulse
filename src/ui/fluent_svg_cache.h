// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>

namespace pulse::ui {
class MainRenderer;
void ClearFluentSvgRasterCache(const MainRenderer* renderer);
size_t FluentSvgRasterCacheBytes();
void SetFluentSvgRasterCacheEnabledForTest(bool enabled);
}
