// SPDX-License-Identifier: Apache-2.0
// Adapted from chenzai666/pulse at fc6dae4a2737c2bc3014972fb24d30219bd7624f.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#include "native_file_dialog_adapter.h"
#include "../common/file_dialog_bridge_bypass.h"

#include <objbase.h>
#include <UIAutomation.h>
#include <dlgs.h>
#include <commctrl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string_view>

namespace pulse::app {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT kMessageTimeout = 250;
constexpr int kMaximumItems = 128;
constexpr size_t kMaximumText = 32768;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct TokenIdentity {
    std::vector<unsigned char> user;
    DWORD integrity = 0;
    DWORD session = 0;
};

struct Controls {
    HWND shell = nullptr;
    HWND direct = nullptr;
    HWND browse = nullptr;
    HWND tree = nullptr;
    HWND filename = nullptr;
    HWND filter = nullptr;
    HWND address = nullptr;
    HWND address_toolbar = nullptr;
    bool ambiguous_filename = false;
    bool ambiguous_filter = false;
    bool standard_filter = false;
    HWND confirm = nullptr;
    HWND cancel = nullptr;
};

bool Fail(std::wstring& reason, const wchar_t* message) {
    reason = message;
    return false;
}

bool Cancelled(const std::function<bool()>& cancelled, std::wstring& reason) {
    if (!cancelled || !cancelled()) return false;
    reason = L"The dialog request was cancelled.";
    return true;
}

std::wstring ClassName(HWND window) {
    wchar_t text[128]{};
    const int length = GetClassNameW(window, text, static_cast<int>(std::size(text)));
    return length > 0 ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

bool Send(HWND window, UINT message, WPARAM wparam, LPARAM lparam, DWORD_PTR& result) {
    if (!window || message >= WM_USER) return false;
    return SendMessageTimeoutW(window, message, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, kMessageTimeout, &result) != 0;
}

bool WindowText(HWND window, std::wstring& result) {
    std::vector<wchar_t> text(kMaximumText);
    DWORD_PTR length = 0;
    if (!Send(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), length) ||
        length >= text.size() - 1) return false;
    result.assign(text.data(), static_cast<size_t>(length));
    return true;
}

std::wstring WindowText(HWND window) {
    std::wstring result;
    WindowText(window, result);
    return result;
}

bool ReadToken(HANDLE process, TokenIdentity& identity) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    identity.user.resize(size);
    if (!GetTokenInformation(token.value, TokenUser, identity.user.data(), size, &size)) return false;
    size = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    std::vector<unsigned char> integrity(size);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, integrity.data(), size, &size)) return false;
    const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data());
    if (!IsValidSid(label->Label.Sid)) return false;
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return false;
    identity.integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
    return GetTokenInformation(token.value, TokenSessionId, &identity.session,
        sizeof(identity.session), &size) != FALSE;
}

bool ProcessTimes(HANDLE process, FILETIME& created) {
    FILETIME exited{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
}

bool WindowIdentity(NativeFileDialog& dialog, std::wstring& reason) {
    const HWND window = dialog.window;
    if (!IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window || ClassName(window) != L"#32770")
        return Fail(reason, L"The source is not a visible standard top-level dialog.");
    dialog.window_thread_id = GetWindowThreadProcessId(window, &dialog.process_id);
    if (!dialog.window_thread_id || dialog.process_id == GetCurrentProcessId())
        return Fail(reason, L"The source is not an external dialog.");
    const DWORD process_id = dialog.process_id;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)};
    if (!process.value || !ProcessTimes(process.value, dialog.process_created))
        return Fail(reason, L"The source process could not be verified.");
    TokenIdentity own, target;
    if (!ReadToken(GetCurrentProcess(), own) || !ReadToken(process.value, target))
        return Fail(reason, L"无法核实进程权限；保留原生对话框。");
    const auto own_user = reinterpret_cast<const TOKEN_USER*>(own.user.data());
    const auto target_user = reinterpret_cast<const TOKEN_USER*>(target.user.data());
    if (!EqualSid(own_user->User.Sid, target_user->User.Sid) || own.session != target.session ||
        own.integrity != target.integrity)
        return Fail(reason, L"用户、会话或完整性级别不一致；不绕过 UIPI。");
    // GetThreadDesktop cannot reliably query threads in other processes on all Windows
    // versions; instead verify the target window is enumerable on the current thread's
    // desktop, which must already have been switched to the correct desktop by the caller.
    const HDESK own_desktop = GetThreadDesktop(GetCurrentThreadId());
    if (!own_desktop) return Fail(reason, L"无法获取当前工作线程的桌面。");
    bool window_on_desktop = false;
    struct DesktopSearch { HWND target; bool& found; };
    DesktopSearch desktop_ctx{window, window_on_desktop};
    EnumDesktopWindows(own_desktop, [](HWND hwnd, LPARAM lp) -> BOOL {
        auto& ctx = *reinterpret_cast<DesktopSearch*>(lp);
        if (hwnd == ctx.target) { ctx.found = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&desktop_ctx));
    if (!window_on_desktop)
        return Fail(reason, L"窗口不在当前工作线程的桌面上。");
    if (!IsWindowEnabled(window)) return Fail(reason, L"原生对话框当前被其他模态窗口阻塞。");
    return true;
}

bool VisibleControl(HWND control, HWND dialog) {
    // Hiding the verified top-level window preserves its children's own visibility styles.
    while (control && control != dialog) {
        if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE)) return false;
        control = GetParent(control);
    }
    return control == dialog;
}

Controls FindControls(HWND window) {
    Controls controls;
    controls.confirm = GetDlgItem(window, IDOK);
    controls.cancel = GetDlgItem(window, IDCANCEL);
    struct Context { Controls& controls; HWND dialog; size_t count = 0; } context{controls, window};
    EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(data);
        if (++state.count > 512) return FALSE;
        auto& result = state.controls;
        const auto name = ClassName(child);
        const bool visible = VisibleControl(child, state.dialog);
        if (name == L"SHELLDLL_DefView") result.shell = child;
        if (name == L"DirectUIHWND") result.direct = child;
        if (name == L"SHBrowseForFolder ShellNameSpace Control") result.browse = child;
        if (name == L"SysTreeView32") result.tree = child;
        const int identifier = GetDlgCtrlID(child);
        if (name == L"ComboBox" && visible && identifier == cmb1) {
            if (result.standard_filter) {
                if (result.filter != child) result.ambiguous_filter = true;
            } else {
                result.filter = child;
                result.standard_filter = true;
                result.ambiguous_filter = false;
            }
        } else if (name == L"ComboBox" && visible && !result.standard_filter &&
            identifier != cmb13 && identifier != 41477 &&
            !FindWindowExW(child, nullptr, L"Edit", nullptr) &&
            (GetWindowLongPtrW(child, GWL_STYLE) & 3) == CBS_DROPDOWNLIST) {
            if (result.filter && result.filter != child) result.ambiguous_filter = true;
            result.filter = child;
        }
        HWND filename = nullptr;
        if ((identifier == edt1 || identifier == cmb13 || identifier == 14148 ||
             (identifier == 1001 && ClassName(GetParent(child)) == L"ComboBox")) &&
            name == L"Edit" && visible)
            filename = child;
        if (identifier == cmb13 && name == L"ComboBox" && visible)
            filename = FindWindowExW(child, nullptr, L"Edit", nullptr);
        if (filename && VisibleControl(filename, state.dialog)) {
            if (result.filename && result.filename != filename) result.ambiguous_filename = true;
            result.filename = filename;
        }
        if (name == L"Edit" && (identifier == 41477 || identifier == 1137)) result.address = child;
        if (name == L"ToolbarWindow32" && identifier == 1001) result.address_toolbar = child;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.count > 512 ? Controls{} : controls;
}

bool Automation(ComPtr<IUIAutomation>& automation, std::wstring& reason) {
    ComPtr<IUIAutomation2> timed;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&timed))) ||
        FAILED(timed->put_ConnectionTimeout(500)) || FAILED(timed->put_TransactionTimeout(500)))
        return Fail(reason, L"无法创建带超时限制的 UI Automation；工作线程须初始化 COM。");
    return SUCCEEDED(timed.As(&automation));
}

ComPtr<IUIAutomationElement> Element(IUIAutomation* automation, HWND window) {
    ComPtr<IUIAutomationElement> element;
    if (window) automation->ElementFromHandle(window, &element);
    return element;
}

std::wstring Name(IUIAutomationElement* element) {
    BSTR name = nullptr;
    if (!element || FAILED(element->get_CurrentName(&name))) return {};
    std::wstring result;
    if (name && SysStringLen(name) < kMaximumText) result.assign(name, SysStringLen(name));
    SysFreeString(name);
    return result;
}

std::wstring ButtonWord(std::wstring_view text) {
    std::wstring result;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'(' && index + 3 < text.size() && text[index + 1] == L'&' &&
            text[index + 3] == L')') { index += 3; continue; }
        if (text[index] != L'&' && !std::iswspace(text[index]))
            result.push_back(static_cast<wchar_t>(std::towlower(text[index])));
    }
    return result;
}

bool AbsolutePath(std::wstring_view path) {
    if (path.empty() || path.size() >= kMaximumText) return false;
    std::wstring extended_unc;
    if (path.size() > 8 && CompareStringOrdinal(path.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL) {
        extended_unc = L"\\\\";
        extended_unc += path.substr(8);
        path = extended_unc;
    } else if (path.starts_with(L"\\\\?\\")) {
        path.remove_prefix(4);
    }
    if (std::any_of(path.begin(), path.end(), [](wchar_t character) {
        return character < L' ' || character == L'"' || character == L'*' || character == L'?' || character == L'/';
    })) return false;
    if (path.size() > 2 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\') return true;
    if (path.size() < 5 || path.substr(0, 2) != L"\\\\" || path[2] == L'.') return false;
    const size_t server_end = path.find(L'\\', 2);
    return server_end != std::wstring_view::npos && server_end > 2 && server_end + 1 < path.size() &&
        path[server_end + 1] != L'\\';
}

std::wstring ParseFilterPattern(const std::wstring& label) {
    const size_t close = label.rfind(L')');
    const size_t open = close == std::wstring::npos ? std::wstring::npos : label.rfind(L'(', close);
    std::wstring pattern = open == std::wstring::npos ? label : label.substr(open + 1, close - open - 1);
    while (!pattern.empty() && std::iswspace(pattern.front())) pattern.erase(pattern.begin());
    while (!pattern.empty() && std::iswspace(pattern.back())) pattern.pop_back();
    if (pattern.empty() || pattern.size() > 4096 || pattern.find_first_of(L"*?") == std::wstring::npos ||
        pattern.find_first_of(L"\\/\r\n\"()") != std::wstring::npos ||
        (open == std::wstring::npos && pattern.find_first_of(L" \t") != std::wstring::npos)) return {};
    return pattern;
}

bool NativeFilters(HWND combo, NativeDialogKind kind, std::vector<ui::PickerFilter>& filters, size_t& selected) {
    if (ClassName(combo) != L"ComboBox") return false;
    const auto style = GetWindowLongPtrW(combo, GWL_STYLE);
    if ((style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) && !(style & CBS_HASSTRINGS)) return false;
    DWORD_PTR count = 0, selection = 0;
    if (!Send(combo, CB_GETCOUNT, 0, 0, count) || count == 0 || count > static_cast<DWORD_PTR>(kMaximumItems) ||
        !Send(combo, CB_GETCURSEL, 0, 0, selection) || selection >= count) return false;
    std::vector<ui::PickerFilter> result;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (DWORD_PTR index = 0; index < count; ++index) {
        if (GetTickCount64() >= deadline) return false;
        // These messages are below WM_USER: user32 marshals their output buffer.
        // Use the full bounded buffer so a changed label cannot overflow it.
        DWORD_PTR length = 0, copied = 0;
        std::vector<wchar_t> label(kMaximumText);
        if (!Send(combo, CB_GETLBTEXTLEN, index, 0, length) || !length || length >= label.size() - 1 ||
            !Send(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()), copied) ||
            copied != length) return false;
        std::wstring text(label.data(), static_cast<size_t>(copied));
        auto pattern = ParseFilterPattern(text);
        // Browser save dialogs may expose just the type label. The original dialog still applies its type and extension.
        if (pattern.empty() && kind != NativeDialogKind::Save) return false;
        result.push_back({std::move(text), std::move(pattern)});
    }
    DWORD_PTR final_count = 0, final_selection = 0;
    if (!Send(combo, CB_GETCOUNT, 0, 0, final_count) || final_count != count ||
        !Send(combo, CB_GETCURSEL, 0, 0, final_selection) || final_selection != selection) return false;
    filters = std::move(result);
    selected = static_cast<size_t>(selection);
    return true;
}

std::wstring Directory(const Controls& controls, IUIAutomation* automation) {
    std::wstring text;
    if (controls.address && WindowText(controls.address, text) && AbsolutePath(text)) return text;
    if (!automation) return {};
    auto toolbar = Element(automation, controls.address_toolbar);
    text = Name(toolbar.Get());
    for (const wchar_t* prefix : {L"Address: ", L"地址: ", L"地址：", L"位址: ", L"位址："}) {
        const std::wstring_view start(prefix);
        if (text.starts_with(start) && AbsolutePath(std::wstring_view(text).substr(start.size())))
            return text.substr(start.size());
    }
    return {};
}

bool MultipleSelection(IUIAutomation* automation, const Controls& controls) {
    const HWND list = FindWindowExW(controls.shell, nullptr, WC_LISTVIEWW, nullptr);
    if (list) return !(GetWindowLongPtrW(list, GWL_STYLE) & LVS_SINGLESEL);
    // Inspect only the verified shell view and its immediate UIA children.
    // A full dialog tree includes namespace providers that can stall traversal.
    if (!automation) return false;
    const HWND direct = FindWindowExW(controls.shell, nullptr, L"DirectUIHWND", nullptr);
    auto view = Element(automation, direct ? direct : controls.shell);
    if (!view) return false;
    ComPtr<IUIAutomationSelectionPattern> pattern;
    BOOL allowed = FALSE;
    if (SUCCEEDED(view->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&pattern))))
        return SUCCEEDED(pattern->get_CurrentCanSelectMultiple(&allowed)) && allowed;
    ComPtr<IUIAutomationCondition> condition;
    VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_ListControlTypeId;
    ComPtr<IUIAutomationElement> child;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition)) ||
        FAILED(view->FindFirst(TreeScope_Children, condition.Get(), &child)) || !child) return false;
    return SUCCEEDED(child->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&pattern))) &&
        SUCCEEDED(pattern->get_CurrentCanSelectMultiple(&allowed)) && allowed;
}

bool IdentityBeforeMutation(const NativeFileDialog& dialog, std::wstring& reason);

bool WriteFilename(const NativeFileDialog& dialog, const std::wstring& text,
    std::wstring& reason, const std::function<bool()>& cancelled) {
    const HWND window = dialog.filename_window;
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason) ||
        ClassName(window) != L"Edit" || (GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY)) return false;
    if (dialog.legacy_folder) {
        // The legacy browse provider also synchronizes the tree/PIDL selection.
        ComPtr<IUIAutomation> automation;
        if (!Automation(automation, reason)) return false;
        auto edit = Element(automation.Get(), window);
        ComPtr<IUIAutomationValuePattern> value;
        if (!edit || FAILED(edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))))
            return Fail(reason, L"原生目录输入框未暴露可写 ValuePattern。");
        BSTR input = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (!input) return Fail(reason, L"无法分配原生目录输入文本。");
        const HRESULT updated = (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason))
            ? E_ABORT : value->SetValue(input);
        SysFreeString(input);
        if (FAILED(updated)) return Fail(reason, L"原生目录 ValuePattern 写入失败。");
    } else {
        // Unlike WM_SETTEXT, a replacement edit emits the normal edit-change
        // notification consumed by the native filename host. user32 marshals
        // EM_REPLACESEL (< WM_USER); text remains alive until Send returns.
        DWORD_PTR result = 0;
        if (!Send(window, EM_SETSEL, 0, static_cast<LPARAM>(-1), result) ||
            Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason) ||
            !Send(window, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()), result))
            return Fail(reason, L"原生文件名输入框未完成标准编辑替换。");
    }
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason)) return false;
    std::wstring actual;
    if (!WindowText(window, actual) || actual != text)
        return Fail(reason, L"原生 Edit 回读与请求文本不一致；未发送确认。");
    return true;
}

bool CommitStandardFilter(const NativeFileDialog& dialog, HWND combo, size_t index,
    std::wstring& reason, const std::function<bool()>& cancelled) {
    const HWND parent = GetParent(combo);
    const auto matches = [&] {
        DWORD process = 0;
        return parent && (parent == dialog.window || IsChild(dialog.window, parent)) &&
            GetParent(combo) == parent && GetDlgCtrlID(combo) == cmb1 &&
            GetWindowThreadProcessId(parent, &process) == dialog.window_thread_id && process == dialog.process_id;
    };
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason) || !matches()) return false;
    DWORD_PTR result = 0;
    if (!Send(combo, CB_SETCURSEL, index, 0, result) || result != index)
        return Fail(reason, L"原生文件类型未切换到选中的位置。");
    // The host may update its extension and type separately from the ComboBox selection.
    for (const auto notification : {CBN_SELENDOK, CBN_SELCHANGE}) {
        if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason) || !matches()) return false;
        if (!Send(parent, WM_COMMAND, MAKEWPARAM(cmb1, notification), reinterpret_cast<LPARAM>(combo), result))
            return Fail(reason, L"原生文件类型变更通知未完成。");
    }
    return (Send(combo, CB_GETCURSEL, 0, 0, result) && result == index) ||
        Fail(reason, L"原生文件类型通知后，选中项发生变化。");
}

bool SelectFilter(const NativeFileDialog& dialog, const Controls& controls, size_t index,
    const std::vector<ui::PickerFilter>& filters, std::wstring& reason,
    const std::function<bool()>& cancelled) {
    if (index >= filters.size() || Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason) ||
        ClassName(controls.filter) != L"ComboBox" || !IsChild(dialog.window, controls.filter))
        return Fail(reason, L"原生文件类型控件身份或目标索引无效。");
    DWORD process = 0;
    if (GetWindowThreadProcessId(controls.filter, &process) != dialog.window_thread_id || process != dialog.process_id)
        return Fail(reason, L"原生文件类型控件进程或线程发生变化。");
    if (controls.standard_filter && !IsWindowVisible(dialog.window))
        return CommitStandardFilter(dialog, controls.filter, index, reason, cancelled);
    ComPtr<IUIAutomation> automation;
    if (!Automation(automation, reason)) return false;
    auto combo = Element(automation.Get(), controls.filter);
    ComPtr<IUIAutomationExpandCollapsePattern> expand;
    if (combo) combo->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&expand));
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason)) return false;
    DWORD_PTR result = 0;
    const bool expanded = expand ? SUCCEEDED(expand->Expand()) : Send(controls.filter, CB_SHOWDROPDOWN, TRUE, 0, result);
    if (!expanded) return Fail(reason, L"原生文件类型 ComboBox 无法展开。");
    struct DropdownGuard {
        const NativeFileDialog& dialog;
        HWND combo;
        bool active = true;
        ~DropdownGuard() {
            if (!active) return;
            std::wstring reason;
            DWORD process = 0;
            if (IdentityBeforeMutation(dialog, reason) && IsChild(dialog.window, combo) &&
                GetWindowThreadProcessId(combo, &process) == dialog.window_thread_id && process == dialog.process_id) {
                DWORD_PTR result = 0;
                Send(combo, CB_SHOWDROPDOWN, FALSE, 0, result);
            }
        }
    } dropdown{dialog, controls.filter};
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(controls.filter, &info) || info.hwndCombo != controls.filter ||
        ClassName(info.hwndList) != L"ComboLBox" ||
        GetWindowThreadProcessId(info.hwndList, &process) != dialog.window_thread_id || process != dialog.process_id)
        return Fail(reason, L"无法核实类型下拉列表的原生 HWND、进程与线程。");
    auto list = Element(automation.Get(), info.hwndList);
    ComPtr<IUIAutomationCondition> condition;
    VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationElementArray> items;
    int count = 0;
    if (!list || FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)) ||
        FAILED(list->FindAll(TreeScope_Children, condition.Get(), &items)) || !items ||
        FAILED(items->get_Length(&count)) || count != static_cast<int>(filters.size()))
        return Fail(reason, L"已展开的类型列表未暴露完整直接子项；不遍历其他 UIA 树。");
    ComPtr<IUIAutomationElement> item;
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    if (FAILED(items->GetElement(static_cast<int>(index), &item)) || Name(item.Get()) != filters[index].label ||
        FAILED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection))))
        return Fail(reason, L"类型列表目标标签或 SelectionItemPattern 不匹配。");
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason)) return false;
    if (FAILED(selection->Select())) return Fail(reason, L"原生文件类型 SelectionItem 选择失败。");
    DWORD_PTR selected = 0;
    if (!Send(controls.filter, CB_GETCURSEL, 0, 0, selected) || selected != index)
        return Fail(reason, L"原生文件类型 SelectionItem 选择后，ComboBox 选中索引仍不一致。");
    if (dialog.kind == NativeDialogKind::Save && controls.standard_filter) {
        if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason)) return false;
        if (!Send(controls.filter, CB_SHOWDROPDOWN, FALSE, 0, result))
            return Fail(reason, L"保存文件类型下拉列表未完成关闭。");
        dropdown.active = false;
        return CommitStandardFilter(dialog, controls.filter, index, reason, cancelled);
    }
    return true;
}

bool SameDirectory(const std::wstring& first, const std::wstring& second) {
    if (first == second) return true;
    Handle a{CreateFileW(first.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    Handle b{CreateFileW(second.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (a.value == INVALID_HANDLE_VALUE || b.value == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION a_info{}, b_info{};
    if (!GetFileInformationByHandle(a.value, &a_info) || !GetFileInformationByHandle(b.value, &b_info) ||
        !(a_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !(b_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        a_info.dwVolumeSerialNumber != b_info.dwVolumeSerialNumber ||
        a_info.nFileIndexHigh != b_info.nFileIndexHigh || a_info.nFileIndexLow != b_info.nFileIndexLow) return false;
    std::wstring a_name(kMaximumText, L'\0'), b_name(kMaximumText, L'\0');
    const DWORD a_size = GetFinalPathNameByHandleW(a.value, a_name.data(), static_cast<DWORD>(a_name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const DWORD b_size = GetFinalPathNameByHandleW(b.value, b_name.data(), static_cast<DWORD>(b_name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return a_size && b_size && a_size < a_name.size() && b_size < b_name.size() &&
        CompareStringOrdinal(a_name.data(), static_cast<int>(a_size), b_name.data(), static_cast<int>(b_size), FALSE) == CSTR_EQUAL;
}

bool SameIdentity(const NativeFileDialog& original, const NativeFileDialog& current) {
    return original.window == current.window && original.process_id == current.process_id &&
        original.window_thread_id == current.window_thread_id && original.kind == current.kind &&
        CompareFileTime(&original.process_created, &current.process_created) == 0 &&
        original.filename_window == current.filename_window && original.confirm_window == current.confirm_window &&
        original.filename_parent == current.filename_parent && original.filename_id == current.filename_id &&
        original.cancel_window == current.cancel_window && original.filter_window == current.filter_window &&
        original.legacy_folder == current.legacy_folder;
}

bool SameThread(HWND window, const NativeFileDialog& dialog) {
    DWORD process_id = 0;
    return dialog.window_thread_id && GetWindowThreadProcessId(window, &process_id) == dialog.window_thread_id &&
        process_id == dialog.process_id;
}

bool IsSourceControl(HWND control, const NativeFileDialog& dialog, std::wstring_view type) {
    return IsChild(dialog.window, control) && SameThread(control, dialog) && ClassName(control) == type;
}

bool IdentityBeforeMutation(const NativeFileDialog& dialog, std::wstring& reason) {
    return (NativeDialogStillMatches(dialog) && IsWindowEnabled(dialog.window)) ||
        Fail(reason, L"The source dialog or its verified controls changed or are disabled.");
}

bool SameFilters(const std::vector<ui::PickerFilter>& original, const std::vector<ui::PickerFilter>& current) {
    if (original.size() != current.size()) return false;
    for (size_t index = 0; index < original.size(); ++index) {
        if (original[index].label != current[index].label || original[index].pattern != current[index].pattern) return false;
    }
    return true;
}

bool SelectionText(const NativeFileDialog& dialog, const std::vector<std::wstring>& paths,
    std::wstring& text, std::wstring& reason) {
    if (paths.empty() || paths.size() > kMaximumItems) return Fail(reason, L"选择结果为空或项目过多。");
    for (const auto& path : paths) {
        if (!AbsolutePath(path)) return Fail(reason, L"仅接受无引号、通配符及控制字符的完整路径。");
    }
    if (paths.size() == 1) { text = paths.front(); return true; }
    if (dialog.kind != NativeDialogKind::Open || !dialog.allow_multiselect)
        return Fail(reason, L"原生对话框未明确允许多选。");
    if (dialog.initial_path.empty()) return Fail(reason, L"无法核实原生当前目录，不能回填相对多选文件名。");
    for (const auto& path : paths) {
        const size_t separator = path.rfind(L'\\');
        if (separator + 1 == path.size()) return Fail(reason, L"多选结果必须包含文件名。");
        const bool drive_root = separator == 2 || (separator == 6 && path.starts_with(L"\\\\?\\") && path[5] == L':');
        if (!SameDirectory(dialog.initial_path, path.substr(0, separator + (drive_root ? 1 : 0))))
            return Fail(reason, L"多选仅支持原生当前目录中的文件；跨目录结果保留原生窗口。");
        if (!text.empty()) text += L' ';
        text += L'"';
        text += path.substr(separator + 1);
        text += L'"';
    }
    if (text.size() >= kMaximumText) return Fail(reason, L"多选文件名表达式过长。");
    return true;
}

bool PostButtonCommand(const NativeFileDialog& dialog, HWND button, int identifier,
    std::wstring& reason, const std::function<bool()>& cancelled) {
    const HWND parent = GetParent(button);
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(dialog, reason)) return false;
    if (GetDlgCtrlID(button) != identifier || !IsWindowEnabled(button) ||
        GetParent(button) != parent || !SameThread(parent, dialog) ||
        (parent != dialog.window && !IsChild(dialog.window, parent)))
        return Fail(reason, L"The native button or its command target changed or is disabled.");
    if (Cancelled(cancelled, reason)) return false;
    // BM_CLICK can be ignored on an inactive dialog. Send its usual notification to the verified parent.
    return PostMessageW(parent, WM_COMMAND, MAKEWPARAM(identifier, BN_CLICKED), reinterpret_cast<LPARAM>(button)) != FALSE ||
        Fail(reason, L"The native button request could not be queued.");
}

} // namespace

bool InspectNativeFileDialog(HWND window, NativeFileDialog& dialog, std::wstring& reason) {
    reason.clear();
    dialog = {};
    NativeFileDialog result;
    result.window = window;
    if (!WindowIdentity(result, reason)) return false;
    const Controls controls = FindControls(window);
    if (!IsSourceControl(controls.confirm, result, L"Button") ||
        !IsSourceControl(controls.cancel, result, L"Button") ||
        !VisibleControl(controls.confirm, window) || !VisibleControl(controls.cancel, window) || !IsWindowEnabled(controls.cancel))
        return Fail(reason, L"The standard confirm or cancel button is unavailable.");
    const bool legacy = controls.browse && controls.tree && !controls.shell && !controls.direct;
    if (!legacy && (!controls.shell || (!controls.direct && !controls.filter)))
        return Fail(reason, L"未识别到标准 DirectUI 或 Explorer 文件对话框结构。");
    if (controls.ambiguous_filename || !IsSourceControl(controls.filename, result, L"Edit") ||
        !VisibleControl(controls.filename, window) || !IsWindowEnabled(controls.filename) ||
        (GetWindowLongPtrW(controls.filename, GWL_STYLE) & ES_READONLY) ||
        !WindowText(controls.filename, result.filename))
        return Fail(reason, L"找不到已核实的标准文件名或目录输入框。");
    auto confirm = WindowText(controls.confirm);
    const auto word = ButtonWord(confirm);
    if (legacy) result.kind = NativeDialogKind::Folder;
    else if (word == L"save" || word == L"保存" || word == L"儲存") result.kind = NativeDialogKind::Save;
    else if (word == L"open" || word == L"打开" || word == L"開啟" || word == L"開啓") result.kind = NativeDialogKind::Open;
    else if (controls.direct && !controls.filter &&
        (word == L"selectfolder" || word == L"选择文件夹" || word == L"選擇資料夾" || word == L"選取資料夾"))
        result.kind = NativeDialogKind::Folder;
    else if (!word.empty() && controls.direct && controls.filter) {
        using Mode = dialog_bridge::NativeMode;
        const auto mode = dialog_bridge::QueryNativeMode(result.process_id, result.window_thread_id, GetWindow(window, GW_OWNER));
        if (mode == Mode::Unknown) return Fail(reason, L"无法可靠识别自定义确认按钮对应的文件对话框模式。");
        result.kind = mode == Mode::Open ? NativeDialogKind::Open : NativeDialogKind::Save;
        result.ok_button_label = std::move(confirm);
    } else return Fail(reason, L"无法可靠区分打开、保存与目录模式；保留原生对话框。");
    if (controls.ambiguous_filter) return Fail(reason, L"存在多个文件类型控件；保留原生窗口。");
    if (result.kind != NativeDialogKind::Folder) {
        if (!IsSourceControl(controls.filter, result, L"ComboBox") ||
            !NativeFilters(controls.filter, result.kind, result.filters, result.filter_index))
            return Fail(reason, L"无法读取完整原生文件类型列表及选中项。");
    } else if (controls.filter) return Fail(reason, L"目录模式与文件类型控件不一致。");
    result.title = WindowText(window);
    ComPtr<IUIAutomation> automation;
    std::wstring automation_reason;
    if (controls.address_toolbar || result.kind == NativeDialogKind::Open)
        Automation(automation, automation_reason);
    result.initial_path = Directory(controls, automation.Get());
    if (result.kind == NativeDialogKind::Open) {
        result.allow_multiselect = !result.initial_path.empty() && MultipleSelection(automation.Get(), controls);
    }
    result.filename_window = controls.filename;
    result.filename_parent = GetParent(controls.filename);
    result.filename_id = GetDlgCtrlID(controls.filename);
    result.confirm_window = controls.confirm;
    result.cancel_window = controls.cancel;
    result.filter_window = controls.filter;
    result.legacy_folder = legacy;
    if (!NativeDialogStillMatches(result))
        return Fail(reason, L"读取过程中原生对话框身份发生变化。");
    dialog = std::move(result);
    return true;
}

bool NativeDialogStillMatches(const NativeFileDialog& dialog) {
    const HWND window = dialog.window;
    if (!dialog.process_id || dialog.process_id == GetCurrentProcessId() ||
        GetAncestor(window, GA_ROOT) != window || ClassName(window) != L"#32770" || !SameThread(window, dialog)) return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dialog.process_id)};
    FILETIME created{};
    return process.value && ProcessTimes(process.value, created) &&
        CompareFileTime(&created, &dialog.process_created) == 0 &&
        IsSourceControl(dialog.filename_window, dialog, L"Edit") &&
        GetParent(dialog.filename_window) == dialog.filename_parent && GetDlgCtrlID(dialog.filename_window) == dialog.filename_id &&
        GetDlgItem(window, IDOK) == dialog.confirm_window && IsSourceControl(dialog.confirm_window, dialog, L"Button") &&
        GetDlgItem(window, IDCANCEL) == dialog.cancel_window && IsSourceControl(dialog.cancel_window, dialog, L"Button") &&
        (!dialog.filter_window || IsSourceControl(dialog.filter_window, dialog, L"ComboBox"));
}

bool PrepareNativeDialogSelection(const NativeFileDialog& dialog, const std::vector<std::wstring>& paths,
    size_t filter_index, NativeFileDialog& prepared, std::wstring& reason,
    const std::function<bool()>& cancelled) {
    reason.clear();
    if (Cancelled(cancelled, reason)) return false;
    NativeFileDialog current;
    if (!InspectNativeFileDialog(dialog.window, current, reason)) return false;
    if (!SameIdentity(dialog, current)) return Fail(reason, L"原生对话框已关闭、替换或改变模式。");
    if (!SameFilters(dialog.filters, current.filters)) return Fail(reason, L"原生过滤器已变化；取消回填。");
    if ((!current.filters.empty() && filter_index >= current.filters.size()) ||
        (current.filters.empty() && filter_index != 0)) return Fail(reason, L"文件类型索引无效。");
    std::wstring text;
    if (!SelectionText(current, paths, text, reason)) return false;
    Controls controls = FindControls(current.window);
    const bool changing_filter = !current.filters.empty() && filter_index != current.filter_index;
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(current, reason)) return false;
    if (changing_filter && !SelectFilter(current, controls, filter_index, current.filters, reason, cancelled))
        return false;
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(current, reason)) return false;
    if (!WriteFilename(current, text, reason, cancelled))
        return false;
    if (Cancelled(cancelled, reason)) return false;
    if (!InspectNativeFileDialog(current.window, prepared, reason)) return false;
    if (!SameIdentity(current, prepared)) return Fail(reason, L"回填后窗口或控件身份发生变化。");
    if (!SameFilters(current.filters, prepared.filters)) return Fail(reason, L"回填后文件类型列表发生变化。");
    if (prepared.filename != text) return Fail(reason, L"回填后原生文件名发生变化。");
    if (prepared.filter_index != filter_index) return Fail(reason, L"回填后原生文件类型选中项发生变化。");
    if (paths.size() > 1 && (!prepared.allow_multiselect || !SameDirectory(current.initial_path, prepared.initial_path)))
        return Fail(reason, L"The native current directory or multiselection support changed.");
    return true;
}

bool ConfirmNativeDialogSelection(const NativeFileDialog& prepared, std::wstring& reason,
    const std::function<bool()>& cancelled, bool& confirmation_attempted) {
    confirmation_attempted = false;
    if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(prepared, reason)) return false;
    std::wstring filename;
    DWORD_PTR filter = 0;
    if (!WindowText(prepared.filename_window, filename) || filename != prepared.filename ||
        (prepared.filter_window && (!Send(prepared.filter_window, CB_GETCURSEL, 0, 0, filter) || filter != prepared.filter_index)))
        return Fail(reason, L"The native filename or filter changed before confirmation.");
    if (prepared.kind == NativeDialogKind::Folder && !prepared.legacy_folder) {
        // The provider submits the typed folder. A bare BN_CLICKED can navigate there instead.
        ComPtr<IUIAutomation> automation;
        if (!Automation(automation, reason)) return false;
        auto button = Element(automation.Get(), prepared.confirm_window);
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (!button || FAILED(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))))
            return Fail(reason, L"The native folder confirmation has no invoke pattern.");
        if (Cancelled(cancelled, reason) || !IdentityBeforeMutation(prepared, reason)) return false;
        confirmation_attempted = true;
        return SUCCEEDED(invoke->Invoke()) || Fail(reason, L"The native folder confirmation could not be invoked.");
    }
    confirmation_attempted = PostButtonCommand(prepared, prepared.confirm_window, IDOK, reason, cancelled);
    return confirmation_attempted;
}

bool CancelNativeDialog(const NativeFileDialog& dialog, std::wstring& reason,
    const std::function<bool()>& cancelled) {
    reason.clear();
    return PostButtonCommand(dialog, dialog.cancel_window, IDCANCEL, reason, cancelled);
}

} // namespace pulse::app
