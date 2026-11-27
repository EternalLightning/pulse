// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FluentTokens.h"
#include "edit_motion.h"
#include "../model/terminal_preferences.h"
#include "smooth_scroll.h"
#include "../ops/terminal_process.h"
#include "ui_compositor.h"
#pragma push_macro("small") // Windows RPC headers define small as char.
#undef small
#include <vterm.h>
#pragma pop_macro("small")
#include <deque>
#include <vector>

namespace pulse::ui {

class Compositor;

class TerminalSurface {
public:
    static bool Supported() { return ops::TerminalProcess::Supported(); }
    ~TerminalSurface();
    void Start(HWND hwnd, const std::wstring& directory, bool isolated, ops::TerminalShell shell, const std::wstring& pwsh_path);
    void Stop();
    void Update();
    void SetOwner(HWND window) { process_.SetOwner(window); }
    UINT_PTR NotificationId() const { return process_.NotificationId(); }
    bool HasForegroundTask() const {
        return status_ == ops::TerminalProcess::Status::Running &&
            (command_pending_ || alternate_screen_ || process_.HasChildProcesses());
    }
    bool Ended() const { return status_ == ops::TerminalProcess::Status::Exited ||
        status_ == ops::TerminalProcess::Status::Failed ||
        status_ == ops::TerminalProcess::Status::ShellNotFound; }
    void StopScroll() { scroll_.Reset(); }
    void Configure(const model::TerminalPreferences& preferences);
    void FollowDirectory(const std::wstring& directory);
    void ResetCaret();
    uint64_t CaretDeadline() const { return caret_deadline_; }
    void ClearCaretDeadline() { caret_deadline_ = 0; }
    void Draw(Compositor& compositor, const Theme& theme, const D2D1_RECT_F& bounds,
              float scale, bool focused, bool resize, bool blink);
    bool Scroll(int wheel_delta, uint64_t now, bool animate);
    bool TickScroll(uint64_t now, bool animate);
    bool Key(UINT key, bool shift, bool alt, bool ctrl, uint64_t now, bool animate);
    void Character(wchar_t character, bool alt);
    void Paste(std::wstring_view text);
    void Select(float x, float y, const D2D1_RECT_F& bounds, bool extend);
    bool HasSelection() const;
    std::wstring SelectedText() const;
    std::wstring LinkAt(float x, float y, const D2D1_RECT_F& bounds) const;
    POINT CursorPoint(const D2D1_RECT_F& bounds) const;
    void DropGraphics();

private:
    friend struct TerminalScrollbackTest;
    static int PushLine(int columns, const VTermScreenCell* cells, void* user);
    static int PopLine(int columns, VTermScreenCell* cells, void* user);
    static int ClearHistory(void* user);
    static int SetProperty(VTermProp prop, VTermValue* value, void* user);
    static void Output(const char* bytes, size_t size, void* user);
    void SendKey(VTermKey key, VTermModifier modifiers = VTERM_MOD_NONE);
    void ResetScroll();
    void ApplyDirectory();
    VTermScreenCell Cell(int row, int column) const;
    int SelectionIndex(float x, float y, const D2D1_RECT_F& bounds) const;
    float MaxScroll() const;

    ops::TerminalProcess process_;
    ops::TerminalProcess::Status status_ = ops::TerminalProcess::Status::Starting;
    VTerm* terminal_ = nullptr;
    VTermScreen* screen_ = nullptr;
    std::deque<std::vector<VTermScreenCell>> history_;
    std::string marker_;
    std::string title_fragment_;
    std::string last_prompt_;
    std::wstring requested_directory_;
    std::wstring pane_directory_;
    bool prompt_ready_ = false;
    bool awaiting_prompt_ = false;
    bool command_pending_ = false;
    bool powershell_ = true;
    bool alternate_screen_ = false;
    bool cursor_visible_ = true;
    bool cursor_blinks_ = true;
    EditCaretBlink caret_blink_;
    uint64_t caret_deadline_ = 0;
    model::TerminalPreferences preferences_;
    wchar_t high_surrogate_ = 0;
    int columns_ = 80;
    int rows_ = 24;
    int anchor_ = -1;
    int active_ = -1;
    float cell_width_ = 8.0f;
    float cell_height_ = 18.0f;
    float scale_ = 0.0f;
    float offset_ = 0;
    SmoothScroll scroll_;
    ComPtr<IDWriteTextFormat> normal_;
    ComPtr<IDWriteTextFormat> bold_;
};

} // namespace pulse::ui
