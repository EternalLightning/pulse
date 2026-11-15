// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../index/content_index.h"

namespace pulse::app {
inline void ConfigureContentSearchOptions(const index::ContentIndexConfig& config,
                                         index::ContentSearchRequest& request) {
    request.maximum_file_bytes = config.maximum_file_bytes;
    request.maximum_document_bytes = config.maximum_document_bytes;
    request.skip_system_locations = true;
}
} // namespace pulse::app
