// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#pragma once

#include "file_picker_dialog.h"

namespace pulse::ui {

enum class SaveTargetState {
    Resolved, Ready, ConfirmOverwrite, InvalidName, InvalidDirectory, InvalidFilter,
    Unsupported, ParentMissing, ParentNotDirectory, TargetDirectory, IoError
};

struct SaveTargetRequest {
    std::wstring directory; // absolute filesystem directory, never the process CWD
    std::wstring filename;  // leaf; explicit no-test policy also accepts strictly parsed paths
    std::vector<PickerFilter> filters;
    size_t filter_index = 0; // zero-based, like the existing picker
    std::wstring default_extension; // original SetDefaultExtension value, without a dot
    bool caller_validates_access = false; // explicit in-process policy, never inferred from filename
    bool filter_changed = false; // user changed type after the initial default was set
    bool no_test_file_create = false; // metadata/overwrite only; never create or test write access
    bool require_existing_save_parent = false;
    bool native_save_defaults = false; // negotiated current-filter extension precedence
    bool allow_legacy_identity = false; // providers without FileIdInfo, with genuine file indexes
};

struct SaveTargetResult {
    SaveTargetState state = SaveTargetState::InvalidName;
    std::wstring path;
    size_t filter_index = 0;
    DWORD error = ERROR_SUCCESS;
    bool target_exists = false;
    bool read_only = false; // independent of overwrite confirmation and caller policy
};

// No filesystem access. Supports ordinary drive/UNC paths and explicit extensions.
// Complex wildcard patterns and trailing dot/space semantics require native fallback.
SaveTargetResult ResolveSaveTarget(const SaveTargetRequest& request);

// Attribute reads only; run off the UI thread. Never creates/truncates a file.
// Ready is not a guarantee of write access. Confirmation cannot prevent later races.
SaveTargetResult ValidateSaveTarget(const SaveTargetRequest& request);

} // namespace pulse::ui
