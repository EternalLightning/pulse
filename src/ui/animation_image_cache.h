// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ui_compositor.h"
#include <vector>

namespace pulse::ui {

bool AnimationPixelCacheEnabled();

struct AnimationImageSlice {
    ID2D1CommandList* commands = nullptr;
    D2D1_RECT_F bounds{};
    ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_RECT_F source{};
};

// Rasterize stable commands once on a separate context. Its private clip stack
// allows cropped textures even when the caller is recording a clipped surface.
class AnimationImageCache {
public:
    bool Build(ID2D1DeviceContext* source, ID2D1CommandList* decoration, D2D1_RECT_F viewport,
               std::vector<AnimationImageSlice>& slices, ComPtr<ID2D1Bitmap1>& background);
    void Reset() { context_.reset(); }
private:
    ComPtr<ID2D1DeviceContext> context_;
};

} // namespace pulse::ui
