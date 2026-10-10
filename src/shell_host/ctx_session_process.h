#pragma once

#include "../ipc/protocol.h"
#include <functional>
#include <memory>
#include <string>

namespace pulse::shell {

inline std::wstring ContextWorkerPipeName(DWORD parent, uint32_t session) {
    return ipc::PipeNameFor(parent) + L"_context_" + std::to_wstring(session);
}

inline bool ContextProcessHasVisibleWindows(DWORD process, bool responsive = false) {
    struct Search {
        DWORD process;
        bool responsive;
        bool found = false;
        static BOOL CALLBACK Visit(HWND window, LPARAM context) {
            auto& search = *reinterpret_cast<Search*>(context);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == search.process && IsWindowVisible(window) &&
                (!search.responsive || !IsHungAppWindow(window))) {
                search.found = true;
                return FALSE;
            }
            return TRUE;
        }
    } search{process, responsive};
    EnumWindows(Search::Visit, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// One killable process per displayed menu. File-operation requests never use it.
class ContextSessionProcesses {
public:
    using Reply = std::function<void(uint32_t, uint32_t, const std::vector<uint8_t>&)>;
    static constexpr size_t kProcessLimit = 4;
    static constexpr DWORD kSessionLifetimeMs = 120000;

    ContextSessionProcesses(std::wstring executable, Reply reply);
    ~ContextSessionProcesses();
    ContextSessionProcesses(const ContextSessionProcesses&) = delete;
    ContextSessionProcesses& operator=(const ContextSessionProcesses&) = delete;
    void Query(uint32_t session, std::vector<uint8_t> payload);
    void Invoke(uint32_t session, uint32_t request, std::vector<uint8_t> payload);
    void Close(uint32_t session);
    void Stop();
    size_t ActiveProcesses() const;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};

} // namespace pulse::shell
