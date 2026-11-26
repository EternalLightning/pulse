// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace pulse::app {

int RunFileDialogBridgeHost(const wchar_t* pipe, const wchar_t* parent_pid, const wchar_t* owner_window,
                            const wchar_t* parent_token);

} // namespace pulse::app
