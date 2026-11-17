// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace pulse::preview {

template <typename ShellThumbnail, typename TextPreview>
bool MakeSvgFallback(bool thumbnail, ShellThumbnail&& shell_thumbnail, TextPreview&& text_preview) {
    return shell_thumbnail() || (!thumbnail && text_preview());
}

} // namespace pulse::preview
