// SPDX-License-Identifier: Apache-2.0
#include "terminal_surface.h"

#include "typography.h"
#include "ui_compositor.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <limits>
#include <utility>

namespace pulse::ui {
namespace {

constexpr size_t kHistoryLines = 1500;
constexpr uint32_t kWideContinuation = (std::numeric_limits<uint32_t>::max)();

void AppendCharacter(std::wstring& text, uint32_t value) {
    if (value < 0x10000) {
        text.push_back(static_cast<wchar_t>(value));
    } else if (value <= 0x10FFFF) {
        value -= 0x10000;
        text.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
        text.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
    }
}

void AppendCell(std::wstring& text, const VTermScreenCell& cell) {
    if (cell.chars[0] == kWideContinuation) return;
    if (!cell.chars[0]) text.push_back(L' ');
    else for (const uint32_t value : cell.chars) {
        if (!value) break;
        AppendCharacter(text, value);
    }
}

VTermScreenCell EmptyCell() {
    VTermScreenCell cell{};
    cell.width = 1;
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    return cell;
}

std::string ToUtf8(std::wstring_view text) {
    std::vector<uint8_t> bytes;
    if (!EncodeUtf8Bytes(std::wstring(text), bytes)) return {};
    return {bytes.begin(), bytes.end()};
}

VTermKey SpecialKey(UINT key) {
    switch (key) {
    case VK_LEFT: return VTERM_KEY_LEFT;
    case VK_RIGHT: return VTERM_KEY_RIGHT;
    case VK_UP: return VTERM_KEY_UP;
    case VK_DOWN: return VTERM_KEY_DOWN;
    case VK_HOME: return VTERM_KEY_HOME;
    case VK_END: return VTERM_KEY_END;
    case VK_PRIOR: return VTERM_KEY_PAGEUP;
    case VK_NEXT: return VTERM_KEY_PAGEDOWN;
    case VK_INSERT: return VTERM_KEY_INS;
    case VK_DELETE: return VTERM_KEY_DEL;
    default:
        if (key >= VK_F1 && key <= VK_F24) return static_cast<VTermKey>(VTERM_KEY_FUNCTION(key - VK_F1 + 1));
        return VTERM_KEY_NONE;
    }
}

D2D1_COLOR_F ReadableConsoleInk(D2D1_COLOR_F foreground, D2D1_COLOR_F background) {
    const float backdrop = RelativeLuminance(background) + 0.05f;
    const auto readable = [backdrop](D2D1_COLOR_F ink) {
        const float luminance = RelativeLuminance(ink) + 0.05f;
        return (std::max)(luminance, backdrop) >= 4.5f * (std::min)(luminance, backdrop);
    };
    if (readable(foreground)) return foreground;
    const auto endpoint = D2D1::ColorF(backdrop >= 0.225f ? D2D1::ColorF::Black : D2D1::ColorF::White);
    float low = 0.0f, high = 1.0f;
    for (int i = 0; i < 8; ++i) {
        const float amount = (low + high) * 0.5f;
        if (readable(BlendOver(WithAlpha(endpoint, amount), foreground))) high = amount;
        else low = amount;
    }
    return BlendOver(WithAlpha(endpoint, high), foreground);
}

bool LinkBoundary(wchar_t ch) {
    return std::iswspace(ch) || std::iswcntrl(ch) || std::wstring_view(L"<>\"'`\\").find(ch) != std::wstring_view::npos;
}

std::wstring HttpLinkAt(std::wstring_view text, size_t hit) {
    if (hit >= text.size() || LinkBoundary(text[hit])) return {};
    size_t start = hit;
    while (start && !LinkBoundary(text[start - 1])) --start;
    size_t end = hit + 1;
    while (end < text.size() && !LinkBoundary(text[end])) ++end;
    while (start < end && std::wstring_view(L"([{").find(text[start]) != std::wstring_view::npos) ++start;
    auto url = text.substr(start, end - start);
    const auto scheme = url.size() >= 8 && _wcsnicmp(url.data(), L"https://", 8) == 0 ? 8u :
                        url.size() >= 7 && _wcsnicmp(url.data(), L"http://", 7) == 0 ? 7u : 0u;
    if (!scheme) return {};
    while (url.size() > scheme) {
        const auto tail = url.back();
        const std::wstring_view closing = L")]}", opening = L"([{";
        const auto bracket = closing.find(tail);
        if (std::wstring_view(L".,;:!?").find(tail) == std::wstring_view::npos &&
            (bracket == std::wstring_view::npos || std::count(url.begin(), url.end(), tail) <=
                std::count(url.begin(), url.end(), opening[bracket]))) break;
        url.remove_suffix(1);
    }
    if (hit < start || hit >= start + url.size() || url.size() == scheme ||
        std::wstring_view(L"/?#:").find(url[scheme]) != std::wstring_view::npos) return {};
    return std::wstring(url);
}

} // namespace

TerminalSurface::~TerminalSurface() { Stop(); }

void TerminalSurface::Start(HWND hwnd, const std::wstring& directory, bool isolated,
                            ops::TerminalShell shell, const std::wstring& pwsh_path) {
    Stop();
    powershell_ = shell != ops::TerminalShell::CommandPrompt;
    terminal_ = vterm_new(rows_, columns_);
    if (!terminal_) {
        status_ = ops::TerminalProcess::Status::Failed;
        return;
    }
    static std::atomic_uint sequence{};
    const std::wstring marker = L"PulseTerminal-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(++sequence) + L":";
    marker_ = ToUtf8(marker);
    vterm_set_utf8(terminal_, 1);
    vterm_output_set_callback(terminal_, Output, this);
    screen_ = vterm_obtain_screen(terminal_);
    static const VTermScreenCallbacks callbacks{
        nullptr, nullptr, nullptr, SetProperty, nullptr, nullptr, PushLine, PopLine, ClearHistory
    };
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, true);
    vterm_screen_reset(screen_, 1);
    awaiting_prompt_ = true;
    pane_directory_ = directory;
    status_ = process_.Start(hwnd, directory, marker, {static_cast<SHORT>(columns_), static_cast<SHORT>(rows_)},
        isolated, shell, pwsh_path)
        ? ops::TerminalProcess::Status::Starting : ops::TerminalProcess::Status::Failed;
}

void TerminalSurface::Stop() {
    process_.Stop();
    if (terminal_) vterm_free(terminal_);
    terminal_ = nullptr;
    screen_ = nullptr;
    history_.clear();
    title_fragment_.clear();
    last_prompt_.clear();
    requested_directory_.clear();
    pane_directory_.clear();
    prompt_ready_ = false;
    awaiting_prompt_ = false;
    command_pending_ = false;
    alternate_screen_ = false;
    cursor_visible_ = cursor_blinks_ = true;
    caret_blink_ = {};
    caret_deadline_ = 0;
    high_surrogate_ = 0;
    ResetScroll();
}

void TerminalSurface::Update() {
    if (!terminal_) return;
    auto update = process_.TakeOutput();
    status_ = update.status;
    if (!update.output.empty()) {
        anchor_ = active_ = -1;
        vterm_input_write(terminal_, update.output.data(), update.output.size());
        vterm_screen_flush_damage(screen_);
    }
    ApplyDirectory();
}

void TerminalSurface::Configure(const model::TerminalPreferences& preferences) {
    if (preferences.font_family != preferences_.font_family || preferences.font_size != preferences_.font_size) {
        DropGraphics();
        anchor_ = active_ = -1;
    }
    if (preferences_.follow_directory && !preferences.follow_directory) {
        requested_directory_.clear();
        pane_directory_.clear();
    }
    preferences_ = preferences;
}

void TerminalSurface::ResetCaret() {
    caret_blink_.Reset(GetTickCount64());
    caret_deadline_ = 0;
}

void TerminalSurface::FollowDirectory(const std::wstring& directory) {
    if (!powershell_ || !preferences_.follow_directory || pulse::path::EqualInsensitive(pane_directory_, directory)) return;
    pane_directory_ = directory;
    requested_directory_ = directory;
    ApplyDirectory();
}

void TerminalSurface::ApplyDirectory() {
    if (!prompt_ready_ || alternate_screen_ || requested_directory_.empty() ||
        status_ != ops::TerminalProcess::Status::Running) return;
    const auto command = ops::TerminalDirectoryInput(requested_directory_);
    if (!command.empty() && process_.Write(command)) {
        requested_directory_.clear();
        prompt_ready_ = false;
        awaiting_prompt_ = true;
    }
}

int TerminalSurface::PushLine(int columns, const VTermScreenCell* cells, void* user) {
    auto& self = *static_cast<TerminalSurface*>(user);
    if (self.history_.size() == kHistoryLines) self.history_.pop_front();
    self.history_.emplace_back(cells, cells + columns);
    if (self.offset_ > 0) {
        self.offset_ = (std::min)(self.offset_ + self.cell_height_, self.MaxScroll());
        self.scroll_.Reset();
    }
    return 1;
}

int TerminalSurface::PopLine(int columns, VTermScreenCell* cells, void* user) {
    auto& self = *static_cast<TerminalSurface*>(user);
    if (self.history_.empty()) return 0;
    const auto& line = self.history_.back();
    std::fill_n(cells, columns, EmptyCell());
    std::copy_n(line.begin(), (std::min)(line.size(), static_cast<size_t>(columns)), cells);
    self.history_.pop_back();
    return 1;
}

int TerminalSurface::ClearHistory(void* user) {
    auto& self = *static_cast<TerminalSurface*>(user);
    self.history_.clear();
    self.ResetScroll();
    return 1;
}

int TerminalSurface::SetProperty(VTermProp prop, VTermValue* value, void* user) {
    auto& self = *static_cast<TerminalSurface*>(user);
    if (prop == VTERM_PROP_CURSORVISIBLE || prop == VTERM_PROP_CURSORBLINK) {
        auto& property = prop == VTERM_PROP_CURSORVISIBLE ? self.cursor_visible_ : self.cursor_blinks_;
        const bool enabled = value->boolean != 0;
        if (enabled != property) self.ResetCaret();
        property = enabled;
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        self.alternate_screen_ = value->boolean != 0;
        self.prompt_ready_ = false;
        self.awaiting_prompt_ = !self.alternate_screen_;
        self.ResetScroll();
    } else if (prop == VTERM_PROP_TITLE) {
        const auto fragment = value->string;
        if (fragment.initial) self.title_fragment_.clear();
        if (fragment.len && self.title_fragment_.size() + fragment.len <= 128)
            self.title_fragment_.append(fragment.str, fragment.len);
        if (fragment.final && self.title_fragment_.starts_with(self.marker_) &&
            self.title_fragment_ != self.last_prompt_) {
            self.last_prompt_ = self.title_fragment_;
            self.command_pending_ = false;
            // A prompt emitted while the user is already typing belongs to
            // the preceding command and cannot safely receive a folder change.
            self.prompt_ready_ = std::exchange(self.awaiting_prompt_, false);
        }
    }
    return 1;
}

void TerminalSurface::Output(const char* bytes, size_t size, void* user) {
    static_cast<TerminalSurface*>(user)->process_.Write({bytes, size});
}

void TerminalSurface::SendKey(VTermKey key, VTermModifier modifiers) {
    ResetCaret();
    if (!terminal_) return;
    prompt_ready_ = false;
    awaiting_prompt_ = key == VTERM_KEY_ENTER;
    if (awaiting_prompt_) command_pending_ = true;
    ResetScroll();
    vterm_keyboard_key(terminal_, key, modifiers);
}

void TerminalSurface::Character(wchar_t character, bool alt) {
    ResetCaret();
    if (!terminal_) return;
    const auto mods = alt ? VTERM_MOD_ALT : VTERM_MOD_NONE;
    switch (character) {
    case L'\r': SendKey(VTERM_KEY_ENTER, mods); return;
    case L'\b': SendKey(VTERM_KEY_BACKSPACE, mods); return;
    case L'\t': SendKey(VTERM_KEY_TAB, mods); return;
    case 27: SendKey(VTERM_KEY_ESCAPE, mods); return;
    default: break;
    }
    if (character >= 0xD800 && character <= 0xDBFF) {
        high_surrogate_ = character;
        return;
    }
    uint32_t value = character;
    if (character >= 0xDC00 && character <= 0xDFFF) {
        if (!high_surrogate_) return;
        value = 0x10000u + ((high_surrogate_ - 0xD800u) << 10u) + (character - 0xDC00u);
    }
    high_surrogate_ = 0;
    prompt_ready_ = false;
    awaiting_prompt_ = value == 3; // Ctrl+C may produce a new shell prompt.
    ResetScroll();
    vterm_keyboard_unichar(terminal_, value, mods);
}

bool TerminalSurface::Key(UINT key, bool shift, bool alt, bool ctrl, uint64_t now, bool animate) {
    if (!terminal_) return false;
    if (shift && !alt && !ctrl && !alternate_screen_ && (key == VK_PRIOR || key == VK_NEXT)) {
        const float page = cell_height_ * static_cast<float>((std::max)(rows_ - 1, 1));
        scroll_.ScrollBy(offset_, key == VK_PRIOR ? page : -page, MaxScroll(), page, now, animate);
        return true;
    }
    const auto vt = SpecialKey(key);
    if (vt == VTERM_KEY_NONE) return false;
    SendKey(vt, static_cast<VTermModifier>((shift ? VTERM_MOD_SHIFT : 0) |
        (alt ? VTERM_MOD_ALT : 0) | (ctrl ? VTERM_MOD_CTRL : 0)));
    return true;
}

void TerminalSurface::Paste(std::wstring_view text) {
    ResetCaret();
    if (!terminal_) return;
    auto bytes = ToUtf8(text);
    if (bytes.empty()) return;
    // A literal escape in clipboard text could end bracketed paste early.
    std::erase(bytes, '\x1b');
    prompt_ready_ = false;
    awaiting_prompt_ = false;
    ResetScroll();
    vterm_keyboard_start_paste(terminal_);
    process_.Write(bytes);
    vterm_keyboard_end_paste(terminal_);
}

void TerminalSurface::ResetScroll() {
    offset_ = 0;
    scroll_.Reset();
    anchor_ = active_ = -1;
}

float TerminalSurface::MaxScroll() const {
    return alternate_screen_ ? 0.0f : static_cast<float>(history_.size()) * cell_height_;
}

bool TerminalSurface::Scroll(int wheel_delta, uint64_t now, bool animate) {
    if (!screen_ || alternate_screen_) return false;
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float page = static_cast<float>(rows_) * cell_height_;
    const float step = lines == WHEEL_PAGESCROLL ? page : static_cast<float>(lines) * cell_height_;
    scroll_.ScrollBy(offset_, static_cast<float>(wheel_delta) * step / WHEEL_DELTA,
        MaxScroll(), page, now, animate);
    return true;
}

bool TerminalSurface::TickScroll(uint64_t now, bool animate) {
    scroll_.Tick(offset_, MaxScroll(), now, animate);
    return scroll_.Pending();
}

VTermScreenCell TerminalSurface::Cell(int row, int column) const {
    if (row < 0 || column < 0 || column >= columns_ || !screen_) return EmptyCell();
    const int history_count = alternate_screen_ ? 0 : static_cast<int>(history_.size());
    if (row < history_count) {
        const auto& line = history_[static_cast<size_t>(row)];
        return static_cast<size_t>(column) < line.size() ? line[static_cast<size_t>(column)] : EmptyCell();
    }
    VTermScreenCell cell = EmptyCell();
    vterm_screen_get_cell(screen_, {row - history_count, column}, &cell);
    return cell;
}

int TerminalSurface::SelectionIndex(float x, float y, const D2D1_RECT_F& bounds) const {
    const int history_count = alternate_screen_ ? 0 : static_cast<int>(history_.size());
    const int row = std::clamp(history_count + static_cast<int>(std::floor((y - bounds.top - offset_) /
        cell_height_)), 0, history_count + rows_ - 1);
    int col = std::clamp(static_cast<int>((x - bounds.left) / cell_width_), 0, columns_ - 1);
    if (col > 0 && Cell(row, col).chars[0] == kWideContinuation) --col;
    return row * columns_ + col;
}

void TerminalSurface::Select(float x, float y, const D2D1_RECT_F& bounds, bool extend) {
    if (!screen_) return;
    if (!extend) scroll_.Reset();
    active_ = SelectionIndex(x, y, bounds);
    if (!extend || anchor_ < 0) anchor_ = active_;
}

bool TerminalSurface::HasSelection() const { return anchor_ >= 0 && active_ >= 0 && active_ != anchor_; }

std::wstring TerminalSurface::SelectedText() const {
    if (!HasSelection()) return {};
    const int first = (std::min)(anchor_, active_);
    const int last = (std::max)(anchor_, active_);
    std::wstring text;
    for (int row = first / columns_; row <= last / columns_; ++row) {
        std::wstring line;
        const int start = row == first / columns_ ? first % columns_ : 0;
        const int end = row == last / columns_ ? last % columns_ : columns_ - 1;
        for (int col = start; col <= end; ++col) AppendCell(line, Cell(row, col));
        if (row != last / columns_) {
            while (!line.empty() && line.back() == L' ') line.pop_back();
            line += L"\r\n";
        }
        text += line;
    }
    return text;
}

std::wstring TerminalSurface::LinkAt(float x, float y, const D2D1_RECT_F& bounds) const {
    using Status = ops::TerminalProcess::Status;
    if (!screen_ || (status_ != Status::Running && status_ != Status::Exited) || x < bounds.left || y < bounds.top ||
        x >= (std::min)(bounds.right, bounds.left + columns_ * cell_width_) ||
        y >= (std::min)(bounds.bottom - (status_ == Status::Exited ? cell_height_ : 0.0f),
                        bounds.top + offset_ + rows_ * cell_height_)) return {};
    const int cell_index = SelectionIndex(x, y, bounds);
    const int row = cell_index / columns_, column = cell_index % columns_;
    const int history_count = alternate_screen_ ? 0 : static_cast<int>(history_.size());
    const auto* state = vterm_obtain_state(terminal_);
    const auto continues = [&](int line) {
        return line >= history_count && line < history_count + rows_ &&
            vterm_state_get_lineinfo(state, line - history_count)->continuation;
    };
    int first = row;
    while (first > 0 && continues(first)) --first;
    std::wstring text;
    size_t hit = 0;
    for (int line = first; line == first || continues(line); ++line) {
        for (int col = 0; col < columns_; ++col) {
            if (line == row && col == column) hit = text.size();
            const auto cell = Cell(line, col);
            if (cell.attrs.conceal) text.push_back(L' ');
            else AppendCell(text, cell);
        }
    }
    return HttpLinkAt(text, hit);
}

POINT TerminalSurface::CursorPoint(const D2D1_RECT_F& bounds) const {
    VTermPos position{};
    if (terminal_) vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &position);
    return {static_cast<LONG>(bounds.left + static_cast<float>(position.col) * cell_width_),
        static_cast<LONG>(bounds.top + static_cast<float>(position.row) * cell_height_)};
}

void TerminalSurface::DropGraphics() {
    normal_.reset();
    bold_.reset();
}

void TerminalSurface::Draw(Compositor& compositor, const Theme& theme, const D2D1_RECT_F& bounds,
                           float scale, bool focused, bool resize, bool blink) {
    caret_deadline_ = 0;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    auto* dc = compositor.Dc();
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(theme.text, &brush))) return;
    const bool high_contrast = IsHighContrast();
    using Status = ops::TerminalProcess::Status;
    using I = l10n::StringId;
    if (status_ == Status::Starting || status_ == Status::Failed || status_ == Status::ShellNotFound) {
        const auto label = l10n::Get(status_ == Status::Starting ? I::TerminalStarting :
            status_ == Status::ShellNotFound ? I::TerminalPwshMissing : I::TerminalStartFailed);
        dc->DrawText(label.c_str(), static_cast<UINT32>(label.size()), compositor.TextFormat(), bounds,
            brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
        return;
    }
    if (!screen_) return;
    auto* factory = compositor.DwriteFactory();
    if (scale_ != scale) DropGraphics();
    if (!normal_.get()) {
        auto spec = typography::Scaled(typography::kDocumentPreview, scale);
        spec.size = static_cast<float>(preferences_.font_size) * (96.0f / 72.0f) * scale;
        const auto* family = preferences_.font_family.c_str();
        typography::CreateTextFormat(factory, spec, &normal_, family);
        spec.weight = DWRITE_FONT_WEIGHT_BOLD;
        typography::CreateTextFormat(factory, spec, &bold_, family);
        if (!normal_.get()) return;
        normal_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (bold_.get()) bold_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        scale_ = scale;
        cell_width_ = (std::max)(1.0f, typography::MeasureAdvance(factory, normal_.get(), L"MMMMMMMMMM") / 10.0f);
        cell_height_ = std::ceil((std::max)(spec.size * 1.35f,
            typography::MeasureWrapped(factory, normal_.get(), L"Mg国", spec.size * 8)));
        ResetScroll();
    }
    auto grid = bounds;
    if (status_ == Status::Exited) grid.bottom -= cell_height_;
    if (resize && status_ == Status::Running) {
        const int cols = std::clamp(static_cast<int>((grid.right - grid.left) / cell_width_), 2, 240);
        const int rows = std::clamp(static_cast<int>((grid.bottom - grid.top) / cell_height_), 2, 140);
        if (rows != rows_ || cols != columns_) {
            columns_ = cols;
            rows_ = rows;
            vterm_set_size(terminal_, rows_, columns_);
            process_.Resize({static_cast<SHORT>(columns_), static_cast<SHORT>(rows_)});
            ResetScroll();
        }
    }
    VTermPos cursor{};
    vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &cursor);
    const int history_count = alternate_screen_ ? 0 : static_cast<int>(history_.size());
    const int first_row = history_count - static_cast<int>(std::ceil(offset_ / cell_height_));
    const int selection_start = HasSelection() ? (std::min)(anchor_, active_) : -1;
    const int selection_end = HasSelection() ? (std::max)(anchor_, active_) : -1;
    const auto color = [&](VTermColor value) {
        vterm_screen_convert_color_to_rgb(screen_, &value);
        return D2D1::ColorF(value.rgb.red / 255.0f, value.rgb.green / 255.0f, value.rgb.blue / 255.0f);
    };
    dc->PushAxisAlignedClip(grid, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int row = first_row; row < history_count + rows_; ++row) {
        const float y = grid.top + offset_ + static_cast<float>(row - history_count) * cell_height_;
        if (y >= grid.bottom) break;
        for (int col = 0; col < columns_;) {
            const auto cell = Cell(row, col);
            if (cell.chars[0] == kWideContinuation) { ++col; continue; }
            const int index = row * columns_ + col;
            const bool plain = !cell.attrs.reverse && !cell.attrs.underline &&
                !cell.attrs.strike && !cell.attrs.conceal && VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
            int count = (std::max)(1, static_cast<int>(cell.width));
            std::wstring text;
            if (!cell.attrs.conceal) AppendCell(text, cell);
            // One draw call for a run of ordinary monospace characters. Complex
            // clusters retain their exact console cell origin and clipping.
            if (plain && cell.chars[0] < 128 && !cell.chars[1]) {
                for (; col + count < columns_; ++count) {
                    const auto next = Cell(row, col + count);
                    if (next.chars[0] >= 128 || next.chars[1] || next.width != 1 ||
                        next.attrs.bold != cell.attrs.bold || next.attrs.conceal ||
                        next.attrs.reverse || next.attrs.underline || next.attrs.strike ||
                        !VTERM_COLOR_IS_DEFAULT_BG(&next.bg) || !vterm_color_is_equal(&cell.fg, &next.fg)) break;
                    AppendCell(text, next);
                }
            }
            const float x = grid.left + static_cast<float>(col) * cell_width_;
            const auto rc = D2D1::RectF(x, y, x + static_cast<float>(count) * cell_width_, y + cell_height_);
            auto fg = high_contrast || VTERM_COLOR_IS_DEFAULT_FG(&cell.fg) ? theme.text : color(cell.fg);
            const auto bg = high_contrast || VTERM_COLOR_IS_DEFAULT_BG(&cell.bg) ? theme.bg : color(cell.bg);
            const auto surface = cell.attrs.reverse ? fg : bg;
            if (cell.attrs.reverse || (!high_contrast && !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg))) {
                brush->SetColor(surface);
                dc->FillRectangle(rc, brush.get());
            }
            if (cell.attrs.reverse) fg = bg;
            const auto original_ink = fg;
            if (!high_contrast && (cell.attrs.reverse ||
                !VTERM_COLOR_IS_DEFAULT_FG(&cell.fg) || !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)))
                fg = ReadableConsoleInk(fg, surface);
            const bool has_text = text.find_first_not_of(L' ') != std::wstring::npos;
            const auto draw = [&](D2D1_COLOR_F ink) {
                brush->SetColor(ink);
                if (has_text)
                    dc->DrawText(text.data(), static_cast<UINT32>(text.size()),
                        cell.attrs.bold && bold_.get() ? bold_.get() : normal_.get(), rc, brush.get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
                if (cell.attrs.underline || cell.attrs.strike) {
                    const float line = cell.attrs.underline ? rc.bottom - 2.0f * scale : rc.top + cell_height_ * 0.55f;
                    dc->DrawLine({rc.left, line}, {rc.right, line}, brush.get(), scale);
                }
            };
            if (selection_start < 0 || selection_end < index || selection_start >= index + count) {
                draw(fg);
            } else {
                const int first = (std::max)(index, selection_start) - index;
                const int end = cell.width > 1 ? count : (std::min)(index + count, selection_end + 1) - index;
                const float left = x + first * cell_width_;
                const float right = x + end * cell_width_;
                const auto highlight = D2D1::RectF(left, rc.top, right, rc.bottom);
                brush->SetColor(theme.fill_selected);
                dc->FillRectangle(highlight, brush.get());
                // Keep DirectWrite's run and origin fixed as the selection moves.
                const auto draw_part = [&](float start, float stop, D2D1_COLOR_F ink) {
                    dc->PushAxisAlignedClip(D2D1::RectF(start, rc.top, stop, rc.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
                    draw(ink);
                    dc->PopAxisAlignedClip();
                };
                if (first) draw_part(rc.left, left, fg);
                draw_part(left, right, high_contrast ? theme.text_selected :
                    ReadableConsoleInk(original_ink, BlendOver(theme.fill_selected, surface)));
                if (end < count) draw_part(right, rc.right, fg);
            }
            col += count;
        }
    }
    if (status_ == Status::Running && focused && cursor_visible_ && offset_ == 0) {
        const auto now = GetTickCount64();
        const UINT interval = blink && cursor_blinks_ ? GetCaretBlinkTime() : UINT32_MAX;
        caret_blink_.Sample(now, false);
        if (const auto delay = caret_blink_.NextDelay(now, interval)) caret_deadline_ = now + delay;
        if (caret_blink_.Visible(now, interval)) {
            const auto point = CursorPoint(grid);
            const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            brush->SetColor(theme.accent_ink);
            dc->FillRectangle(D2D1::RectF(x, y + scale, x + 2.0f * scale, y + cell_height_ - scale), brush.get());
        }
    } else {
        caret_blink_ = {};
    }
    dc->PopAxisAlignedClip();
    if (status_ == Status::Exited) {
        auto status = bounds;
        status.top = grid.bottom;
        const auto label = l10n::Get(I::TerminalExited);
        brush->SetColor(theme.text_secondary);
        dc->DrawText(label.c_str(), static_cast<UINT32>(label.size()), compositor.SmallFormat(),
            status, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
    }
}

} // namespace pulse::ui
