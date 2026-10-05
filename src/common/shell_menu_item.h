#pragma once
#include <cstdint>
#include <string>

namespace pulse {

// One Explorer verb from a Shell context-menu session. Submenus use a header
// row (has_children) followed by child rows, with one level of nesting.
struct ShellMenuItem {
    uint32_t id = 0;             // host menu command id; pass back to Invoke
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;   // submenu header; id is not invokable
    bool child = false;
    std::wstring verb;           // canonical verb (may be empty)
    std::wstring text;
    std::wstring clsid;
    std::wstring handler;
};

} // namespace pulse
