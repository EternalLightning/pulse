#include "../ui/operation_center.h"
#include "../common/localization.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace pulse::ui {
struct OperationCenterTestPeer {
    static bool SaveDib(const OperationCenter& center, const std::filesystem::path& path) {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (!center.bits_ || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(static_cast<UINT>(center.dib_width_), static_cast<UINT>(center.dib_height_)))) return false;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        return SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WritePixels(static_cast<UINT>(center.dib_height_), static_cast<UINT>(center.dib_width_ * 4),
                static_cast<UINT>(center.dib_width_ * center.dib_height_ * 4), static_cast<BYTE*>(center.bits_))) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    static int Run(OperationCenter& center, const std::filesystem::path& output) {
        int failures = 0;
        auto check = [&](bool passed, const char* label) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
            if (!passed) ++failures;
        };
        uint64_t details = 0, cancelled = 0, paused = 0, resumed = 0;
        int clears = 0, pause_calls = 0;
        std::wstring destination;
        OperationCenterCallbacks callbacks;
        callbacks.clear_finished = [&] { ++clears; };
        callbacks.details = [&](uint64_t id) { details = id; };
        callbacks.cancel = [&](uint64_t id) { cancelled = id; };
        callbacks.pause = [&](uint64_t id) { paused = id; ++pause_calls; };
        callbacks.resume = [&](uint64_t id) { resumed = id; };
        callbacks.open_destination = [&](const auto& path) { destination = path; };
        center.SetCallbacks(callbacks);
        OperationCenterLabels labels;
        labels.details = L"详细信息";
        center.SetLabels(std::move(labels));
        center.scale_ = 1;
        center.painter_.SetScale(1);
        center.Update({});
        check(center.SaveDebugSnapshot((output / L"empty-light.png").c_str()), "empty state renders offscreen without activation");
        check(center.cards_.empty() && center.buttons_.empty(), "empty state has no stale actions");

        ops::OpStatus queued;
        queued.task_id = 9; queued.type = ops::OpType::Move; queued.phase = ops::OpPhase::Queued;
        queued.source_label = L"C:\\fixture\\queued source.txt";
        queued.destination_label = L"C:\\fixture\\destination";
        ops::OpStatus running = queued;
        running.task_id = 7; running.active = true; running.phase = ops::OpPhase::Running;
        running.percent = 43; running.total_items = 21; running.completed_items = 9;
        running.bytes_per_second = 54 * 1024 * 1024; running.eta_seconds = 62;
        running.summary = L"正在复制 · Copying 21 files";
        running.current_item = L"C:\\fixture\\deep\\a-long-current-file-name-to-ellipsize-123456789012345678901234567890.txt";
        ops::OpStatus paused_task = running;
        paused_task.task_id = 8; paused_task.phase = ops::OpPhase::Paused;
        ops::OpStatus failed = running;
        failed.task_id = 2; failed.active = false; failed.phase = ops::OpPhase::Failed;
        failed.last_error = L"无法访问目标位置：文件正在被其他进程使用。Access denied for a very long destination. Details remain above the action buttons at every width.";
        ops::OpStatus completed = running;
        completed.task_id = 3; completed.active = false; completed.phase = ops::OpPhase::Completed; completed.percent = 100;
        ops::OpStatus shell = running;
        shell.task_id = 6; shell.type = ops::OpType::RecycleDelete;
        MONITORINFO opening_monitor{sizeof(opening_monitor)};
        GetMonitorInfoW(MonitorFromWindow(center.owner_, MONITOR_DEFAULTTONEAREST), &opening_monitor);
        const RECT opening_anchor{opening_monitor.rcWork.right - 100, opening_monitor.rcWork.top + 60,
            opening_monitor.rcWork.right - 60, opening_monitor.rcWork.top + 100};
        center.Update({queued});
        center.Position(opening_anchor);
        check(center.height_ < static_cast<int>(std::ceil((600 + 2 * kMarginForTest) * center.scale_)) &&
            center.body_height_ >= center.content_height_,
            "single queued task initially fits its content without a full-height empty panel");
        center.Update({failed, completed, queued, running, paused_task, shell});
        center.Position(opening_anchor);
        check(center.height_ <= static_cast<int>(std::ceil((600 + 2 * kMarginForTest) * center.scale_)) &&
            center.height_ <= opening_monitor.rcWork.bottom - opening_monitor.rcWork.top &&
            center.content_height_ > center.body_height_,
            "multiple tasks respect the opening height and monitor limits and retain scrolling");
        const int expanded_height = center.height_;
        center.open_ = true; center.visibility_ = 1;
        center.animation_from_ = 1; center.animation_start_ = GetTickCount64() - 1000;
        center.Scroll(1e6f);
        auto clearing_callbacks = callbacks;
        clearing_callbacks.clear_finished = [&] { ++clears; center.Update({}); };
        center.SetCallbacks(clearing_callbacks);
        check(center.Render() && SaveDib(center, output / L"resize-expanded.png"), "save expanded panel before clearing");
        center.Invoke(0);
        const int empty_height = (std::min)(static_cast<int>(std::ceil((210 + 2 * kMarginForTest) * center.scale_)),
            static_cast<int>(opening_monitor.rcWork.bottom - opening_monitor.rcWork.top));
        check(clears == 1 && center.tasks_.empty() && center.target_height_ == empty_height && center.scroll_ == 0,
            "clearing finished rows retargets the visible popup to its default empty height and resets scrolling");
        check(center.AnimationsEnabled() ? center.resizing_ && center.height_ == expanded_height : center.height_ == empty_height,
            "clear begins from the displayed height or settles immediately when motion is disabled");
        center.resize_start_ = GetTickCount64() - 100;
        center.card_animation_start_ = GetTickCount64() - 100;
        const auto resize_start = center.resize_start_;
        center.Update({});
        check(center.resize_start_ == resize_start, "repeated status updates do not restart the same height transition");
        center.Tick();
        check(center.AnimationsEnabled() ? center.height_ > empty_height && center.height_ < expanded_height : center.height_ == empty_height,
            "height passes through an intermediate frame while shrinking");
        check(SaveDib(center, output / L"resize-midpoint.png"), "save intermediate height transition");
        const int intermediate_height = center.height_;
        center.Update({failed, completed, queued, running, paused_task, shell});
        check(center.target_height_ == expanded_height &&
            (center.AnimationsEnabled() ? center.height_ == intermediate_height : center.height_ == expanded_height),
            "new work reverses the transition continuously from the displayed height");
        center.Update({});
        center.resize_start_ = GetTickCount64() - 250;
        center.card_animation_start_ = GetTickCount64() - 250;
        center.Tick();
        check(center.height_ == empty_height && !center.resizing_ && center.buttons_.empty(),
            "clear settles at the default height with no stale card actions");
        check(center.base_y_ >= opening_monitor.rcWork.top && center.base_y_ + center.height_ <= opening_monitor.rcWork.bottom,
            "resizing retains the monitor work area bounds");
        check(SaveDib(center, output / L"resize-empty.png"), "save the compact empty panel after clearing");
        center.Hide(false); clears = 0;
        center.SetCallbacks(callbacks);
        center.Update({failed, completed, queued, running, paused_task, shell});
        center.Position(opening_anchor);
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            center.scale_ = scale; center.painter_.SetScale(scale);
            center.compositor_->RecreateTextFormats(scale);
            for (const bool dark : {false, true}) for (const int width : {472, 280}) {
                center.SetTheme(dark, HexColor(0x0078D4));
                const auto name = L"cards-" + std::to_wstring(static_cast<int>(scale * 100)) + L"-" +
                    std::to_wstring(width) + (dark ? L"-dark.png" : L"-light.png");
                check(center.SaveDebugSnapshot((output / name).c_str(), width, 520), "mixed tasks render at narrow width, theme and DPI");
                check(center.tasks_[0].task_id == 7 && center.tasks_[3].task_id == 9 && center.tasks_[4].task_id == 3 &&
                    center.tasks_[5].task_id == 2, "current then queued FIFO then newest recent tasks");
                bool safe = true, has_pause = false, has_resume = false, details_in_header = true, actions_right_aligned = true;
                for (const auto& button : center.buttons_) {
                    const bool control = button.action == OperationCenter::Action::Pause ||
                        button.action == OperationCenter::Action::Resume || button.action == OperationCenter::Action::Cancel;
                    if (control && (button.task_id != 7 && button.task_id != 8)) safe = false;
                    has_pause |= button.action == OperationCenter::Action::Pause && button.task_id == 7;
                    has_resume |= button.action == OperationCenter::Action::Resume && button.task_id == 8;
                    if (button.action == OperationCenter::Action::Clear) continue;
                    const auto card = std::find_if(center.cards_.begin(), center.cards_.end(), [&](const auto& item) {
                        return center.tasks_[item.task].task_id == button.task_id;
                    });
                    if (card == center.cards_.end()) { safe = false; continue; }
                    const float offset = (kMarginForTest + 56) * scale - center.scroll_;
                    safe &= button.bounds.left >= card->bounds.left && button.bounds.right <= card->bounds.right;
                    safe &= button.bounds.top >= card->bounds.top + offset && button.bounds.bottom <= card->bounds.bottom + offset;
                    if (button.action == OperationCenter::Action::Details)
                        details_in_header &= button.bounds.top >= card->bounds.top + offset &&
                            button.bounds.bottom <= card->bounds.top + offset + 40 * scale &&
                            card->title_right + 8 * scale <= button.bounds.left;
                    else {
                        float row_right = button.bounds.right;
                        for (const auto& peer : center.buttons_)
                            if (peer.task_id == button.task_id && peer.bounds.top == button.bounds.top)
                                row_right = (std::max)(row_right, peer.bounds.right);
                        actions_right_aligned &= std::abs(row_right - (card->bounds.right - 12 * scale)) < 0.01f;
                    }
                }
                check(safe && has_pause && has_resume, "actions stay inside cards and only active transfers get controls");
                check(details_in_header, "details shares the title row on the right without overlapping the title at each width and DPI");
                check(actions_right_aligned, "every footer action row is right aligned including wrapped rows at each width and DPI");
                check(std::none_of(center.buttons_.begin(), center.buttons_.end(), [&](const auto& button) {
                    return button.task_id == queued.task_id && button.action == OperationCenter::Action::Details;
                }), "queued cards do not open a progress dialog with incompatible state semantics");
                bool overlap = false;
                for (size_t i = 0; i < center.buttons_.size(); ++i) for (size_t j = i + 1; j < center.buttons_.size(); ++j) {
                    const auto& a = center.buttons_[i].bounds; const auto& b = center.buttons_[j].bounds;
                    if (a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top) overlap = true;
                }
                check(!overlap, "buttons wrap without overlap at narrow width");
                bool content_order = true;
                bool equal_insets = true;
                for (const auto& card : center.cards_) {
                    equal_insets &= std::abs(card.bounds.left - (kMarginForTest + 16) * scale) < 0.01f &&
                        std::abs((width + kMarginForTest) * scale - card.bounds.right - 16 * scale) < 0.01f;
                    if (card.source_y && card.destination_y)
                        content_order &= card.paired_paths ? card.source_y == card.destination_y
                            : card.destination_y >= card.source_y + 24 * scale;
                    if (card.progress_y && card.statistics_y)
                        content_order &= card.statistics_y >= card.progress_y + 24 * scale;
                    if (card.footer_y && card.statistics_y) {
                        const auto& task = center.tasks_[card.task];
                        const float stats_height = task.active && (task.type == ops::OpType::Copy || task.type == ops::OpType::Move)
                            ? (card.statistics_columns == 3 ? 56.0f : 100.0f) : 24.0f;
                        content_order &= card.footer_y >= card.statistics_y + stats_height * scale;
                    }
                    if (center.tasks_[card.task].phase == ops::OpPhase::Completed)
                        content_order &= card.item_y == 0 && card.progress_y == 0;
                }
                check(content_order, "routes stats and footer keep separate space and completed cards omit transient progress");
                check(equal_insets, "cards share the title and section inset with equal margins on both sides at every width and DPI");
            }
        }
        center.scale_ = 1; center.painter_.SetScale(1); center.compositor_->RecreateTextFormats(1);
        for (const auto& task : {running, paused_task, failed, completed, queued}) {
            center.Update({task});
            for (const bool dark : {false, true}) {
                center.SetTheme(dark, HexColor(0x0078D4));
                const auto name = L"single-" + std::to_wstring(static_cast<int>(task.phase)) + (dark ? L"-dark.png" : L"-light.png");
                check(center.SaveDebugSnapshot((output / name).c_str(), 472, 400), "single task state renders for visual review");
            }
        }
        center.Update({failed, completed, queued, running, paused_task, shell});
        center.width_ = 496; center.height_ = 440; center.Layout();
        check(center.content_height_ > center.body_height_, "mixed task list requires scrolling");
        center.Scroll(1e6f);
        check(center.scroll_ == center.content_height_ - center.body_height_, "wheel scroll clamps at the final card");
        center.Scroll(-1e6f);
        check(center.scroll_ == 0, "reverse wheel clamps at start");
        center.focused_ = -1; center.FocusButton(-1);
        check(center.focused_ == static_cast<int>(center.buttons_.size()) - 1 && center.scroll_ > 0,
            "Shift Tab starts at last action and reveals it");
        center.FocusButton(1);
        check(center.focused_ == 0, "Tab wraps to header action");
        SendMessageW(center.hwnd_, WM_KEYDOWN, VK_RETURN, 0);
        check(clears == 1, "Enter activates the focused action through the popup message handler");
        clears = 0;
        const auto invoke = [&](OperationCenter::Action action, uint64_t task) {
            for (size_t i = 0; i < center.buttons_.size(); ++i)
                if (center.buttons_[i].action == action && center.buttons_[i].task_id == task) { center.Invoke(static_cast<int>(i)); return true; }
            return false;
        };
        check(invoke(OperationCenter::Action::Pause, 7) && paused == 7 &&
            invoke(OperationCenter::Action::Resume, 8) && resumed == 8 &&
            invoke(OperationCenter::Action::Cancel, 7) && cancelled == 7,
            "pause resume and cancel callbacks carry exact task IDs");
        check(invoke(OperationCenter::Action::Details, 2) && details == 2,
            "failed task opens explicit details without automatic retry");
        check(invoke(OperationCenter::Action::Destination, 7) && destination == running.destination_label,
            "destination callback carries the validated absolute path");
        check(invoke(OperationCenter::Action::Clear, 0) && clears == 1, "clear finished callback dispatches once");
        center.Update({running}); center.Layout();
        center.open_ = true; center.closing_ = false;
        const auto pause_button = std::find_if(center.buttons_.begin(), center.buttons_.end(), [](const auto& button) {
            return button.action == OperationCenter::Action::Pause;
        });
        check(pause_button != center.buttons_.end(), "running transfer supplies a pause button for update-during-click regression");
        if (pause_button != center.buttons_.end()) {
            const int x = static_cast<int>((pause_button->bounds.left + pause_button->bounds.right) * 0.5f);
            const int y = static_cast<int>((pause_button->bounds.top + pause_button->bounds.bottom) * 0.5f);
            SendMessageW(center.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            running.percent = 44;
            center.Update({running});
            check(center.pressed_ >= 0 && center.buttons_[center.pressed_].task_id == running.task_id &&
                center.buttons_[center.pressed_].action == OperationCenter::Action::Pause && center.focused_ == center.pressed_,
                "progress update preserves a held button and keyboard focus by task ID and action");
            paused = 0;
            const int calls_before_release = pause_calls;
            SendMessageW(center.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            check(paused == running.task_id && pause_calls == calls_before_release + 1,
                "mouse release after a progress update invokes the held action exactly once");
        }
        center.Hide(false);
        {
            auto nested = failed;
            nested.task_id = 12;
            nested.current_item = L"C:\\fixture\\source-folder\\unfinished-child.txt";
            auto unsafe = failed;
            unsafe.task_id = 13;
            auto cancelled_task = failed;
            cancelled_task.task_id = 14;
            cancelled_task.phase = ops::OpPhase::Cancelled;
            bool tray_failed = true, continue_failed = true, continue_nested = true;
            int tray_calls = 0, continue_calls = 0;
            uint64_t tray_task = 0, continued_task = 0;
            auto recovery_callbacks = callbacks;
            recovery_callbacks.can_return_to_tray = [&](uint64_t id) {
                return (id == failed.task_id && tray_failed) || id == cancelled_task.task_id ||
                    id == running.task_id || id == queued.task_id;
            };
            recovery_callbacks.can_resume_incomplete = [&](uint64_t id) {
                return (id == failed.task_id && continue_failed) || (id == nested.task_id && continue_nested) ||
                    id == running.task_id || id == queued.task_id;
            };
            recovery_callbacks.return_to_tray = [&](uint64_t id) {
                ++tray_calls; tray_task = id;
                tray_failed = continue_failed = false;
                center.Update(center.tasks_); center.Layout();
            };
            recovery_callbacks.resume_incomplete = [&](uint64_t id) {
                ++continue_calls; continued_task = id;
                continue_nested = false;
                center.Update(center.tasks_); center.Layout();
            };
            center.SetCallbacks(recovery_callbacks);
            center.Update({running, queued, failed, nested, unsafe, completed, cancelled_task});
            center.scroll_ = 0; center.Layout();
            const auto find_recovery = [&](OperationCenter::Action action, uint64_t id) {
                for (size_t i = 0; i < center.buttons_.size(); ++i)
                    if (center.buttons_[i].action == action && center.buttons_[i].task_id == id) return static_cast<int>(i);
                return -1;
            };
            check(find_recovery(OperationCenter::Action::ReturnToTray, failed.task_id) >= 0 &&
                find_recovery(OperationCenter::Action::ReturnToTray, cancelled_task.task_id) >= 0 &&
                find_recovery(OperationCenter::Action::ReturnToTray, unsafe.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, unsafe.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ReturnToTray, completed.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, completed.task_id) < 0,
                "only terminal snapshots with a current safe recovery offer expose recovery buttons");
            check(find_recovery(OperationCenter::Action::ReturnToTray, running.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, running.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ReturnToTray, queued.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, queued.task_id) < 0,
                "active and queued tasks never expose terminal recovery even when capability predicates return true");
            check(find_recovery(OperationCenter::Action::ReturnToTray, nested.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, nested.task_id) >= 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, failed.task_id) < 0,
                "nested unfinished items expose continue only and tray recovery takes priority when both offers are eligible");
            const int stale_tray = find_recovery(OperationCenter::Action::ReturnToTray, failed.task_id);
            tray_failed = false;
            center.Invoke(stale_tray);
            check(stale_tray >= 0 && tray_calls == 0,
                "a previously rendered tray button rechecks live eligibility before dispatch");
            const int stale_continue = find_recovery(OperationCenter::Action::ResumeIncomplete, nested.task_id);
            continue_nested = false;
            center.Invoke(stale_continue);
            check(stale_continue >= 0 && continue_calls == 0,
                "a previously rendered continue button cannot dispatch after its offer is revoked");
            tray_failed = continue_nested = true;
            center.Layout();
            for (const float scale : {1.0f, 1.5f}) {
                center.scale_ = scale; center.painter_.SetScale(scale); center.compositor_->RecreateTextFormats(scale);
                const auto path = output / (L"recovery-" + std::to_wstring(static_cast<int>(scale * 100)) + L"-280-light.png");
                check(center.SaveDebugSnapshot(path.c_str(), 280, 520), "recovery labels render at narrow width and DPI without activation");
                bool safe = true;
                for (const auto& button : center.buttons_) {
                    if (button.action == OperationCenter::Action::Clear) continue;
                    const auto card = std::find_if(center.cards_.begin(), center.cards_.end(), [&](const auto& candidate) {
                        return center.tasks_[candidate.task].task_id == button.task_id;
                    });
                    if (card == center.cards_.end()) { safe = false; continue; }
                    const float offset = (kMarginForTest + 56) * scale - center.scroll_;
                    safe &= button.bounds.left >= card->bounds.left && button.bounds.right <= card->bounds.right &&
                        button.bounds.top >= card->bounds.top + offset && button.bounds.bottom <= card->bounds.bottom + offset;
                }
                for (size_t i = 0; i < center.buttons_.size(); ++i) for (size_t j = i + 1; j < center.buttons_.size(); ++j) {
                    const auto& a = center.buttons_[i].bounds; const auto& b = center.buttons_[j].bounds;
                    safe &= !(a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top);
                }
                check(safe, "long tray and continue actions wrap inside their cards without overlapping other controls");
            }
            center.scale_ = 1; center.painter_.SetScale(1); center.compositor_->RecreateTextFormats(1);
            center.Layout();
            const auto click_recovery = [&](OperationCenter::Action action, uint64_t id) {
                const int index = find_recovery(action, id);
                if (index < 0) return false;
                const auto before = center.buttons_[index].bounds;
                const float top = (kMarginForTest + 56) * center.scale_;
                const float bottom = center.height_ - kMarginForTest * center.scale_;
                if (before.top < top) center.Scroll(before.top - top - 4 * center.scale_);
                else if (before.bottom > bottom) center.Scroll(before.bottom - bottom + 4 * center.scale_);
                const auto rect = center.buttons_[index].bounds;
                const int x = static_cast<int>((rect.left + rect.right) * 0.5f);
                const int y = static_cast<int>((rect.top + rect.bottom) * 0.5f);
                SendMessageW(center.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(center.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
                return true;
            };
            check(click_recovery(OperationCenter::Action::ReturnToTray, failed.task_id) && tray_calls == 1 && tray_task == failed.task_id &&
                find_recovery(OperationCenter::Action::ReturnToTray, failed.task_id) < 0 &&
                find_recovery(OperationCenter::Action::ResumeIncomplete, failed.task_id) < 0,
                "one tray invocation refreshes the consumed offer away and carries the exact task ID");
            check(!invoke(OperationCenter::Action::ReturnToTray, failed.task_id) && tray_calls == 1,
                "consumed tray recovery cannot be invoked a second time from the refreshed panel");
            check(click_recovery(OperationCenter::Action::ResumeIncomplete, nested.task_id) && continue_calls == 1 &&
                continued_task == nested.task_id && find_recovery(OperationCenter::Action::ResumeIncomplete, nested.task_id) < 0,
                "one nested continuation refreshes its consumed offer away and carries the exact task ID");
            check(!invoke(OperationCenter::Action::ResumeIncomplete, nested.task_id) && continue_calls == 1,
                "consumed nested continuation cannot be invoked a second time from the refreshed panel");
            center.SetCallbacks(callbacks);
        }
        shell.destination_label = L"not a real path";
        shell.source_label.clear(); shell.summary.clear(); shell.current_item.clear();
        center.Update({shell}); center.Layout();
        check(center.buttons_.size() == 1 && center.buttons_[0].action == OperationCenter::Action::Details,
            "Shell task has no transfer controls or meaningless destination action");
        check(center.cards_.size() == 1 && center.cards_[0].footer_y == 0 &&
            center.cards_[0].bounds.bottom - center.cards_[0].bounds.top == 126 * center.scale_,
            "a header-only details action adds no footer action row");
        std::vector<ops::OpStatus> history;
        for (uint64_t id = 1; id <= 80; ++id) { auto item = completed; item.task_id = id; history.push_back(item); }
        center.Update(std::move(history)); center.Layout();
        check(center.tasks_.size() == 64 && center.tasks_.front().task_id == 80 && center.tasks_.back().task_id == 17,
            "recent terminal history is bounded to the newest 64 entries");
        center.force_solid_ = true;
        check(center.Render() && !center.acrylic_, "disabled material uses the production solid fallback");
        center.force_solid_ = false;
        center.Update({running}); center.Layout();
        if (MenuBackdropSource::Enabled()) {
            const HWND foreground = GetForegroundWindow();
            SetWindowPos(center.owner_, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(center.owner_, SW_SHOWNOACTIVATE);
            RECT owner_rect{};
            GetWindowRect(center.owner_, &owner_rect);
            const bool offscreen = MonitorFromRect(&owner_rect, MONITOR_DEFAULTTONULL) == nullptr;
            check(offscreen, "material fixture stays wholly outside every monitor");
            if (offscreen) {
                auto* dc = center.compositor_->Dc();
                ComPtr<ID2D1SolidColorBrush> brush;
                bool painted = SUCCEEDED(dc->CreateSolidColorBrush(HexColor(0x547EA8), &brush));
                if (painted) {
                    dc->BeginDraw();
                    dc->Clear(HexColor(0xB0D8EC));
                    const auto frame = D2D1::RectF(0, 0, static_cast<float>(center.compositor_->Width()),
                        static_cast<float>(center.compositor_->Height()));
                    for (int stripe = 0; stripe < 24; ++stripe) {
                        brush->SetColor(stripe % 2 ? HexColor(0x14599A) : HexColor(0xD55A8E));
                        dc->FillRectangle(D2D1::RectF(stripe * 40.0f, frame.top, stripe * 40.0f + 20, frame.bottom), brush.get());
                    }
                    painted = SUCCEEDED(dc->EndDraw()) && center.compositor_->CaptureFrame();
                }
                check(painted, "material source is the fixture's own patterned GPU frame captured before Present");
                POINT origin{};
                ClientToScreen(center.owner_, &origin);
                center.base_x_ = origin.x; center.base_y_ = origin.y;
                center.SetTheme(false, HexColor(0x0078D4));
                center.backdrop_.Reset(); center.backdrop_source_.Reset();
                // Explicitly disable desktop capture while allowing the real
                // material renderer to consume the fixture's own FrameCopy.
                const bool rendered = painted && center.Render(false);
                check(rendered && center.acrylic_ && center.backdrop_.Active() &&
                    !center.backdrop_source_.Bitmap(), "production acrylic branch uses only the fixture FrameCopy");
                check(rendered && center.acrylic_ && SaveDib(center, output / L"material-acrylic-framecopy.png"),
                    "save real sampled acrylic evidence without snapshot solid override");
                for (const bool dark : {false, true}) {
                    std::array<int, 3> first_pixel{};
                    bool translucent = true;
                    for (int sample = 0; sample < 2; ++sample) {
                        dc->BeginDraw();
                        dc->Clear(HexColor(sample ? 0x184EC4 : 0xE86438));
                        const bool captured = SUCCEEDED(dc->EndDraw()) && center.compositor_->CaptureFrame();
                        center.SetTheme(dark, HexColor(0x0078D4));
                        center.backdrop_.Reset(); center.backdrop_source_.Reset();
                        const bool painted_card = captured && center.Render(false) && center.acrylic_;
                        translucent &= painted_card && !center.cards_.empty();
                        if (!painted_card || center.cards_.empty()) continue;
                        const auto& card = center.cards_.front();
                        const int x = static_cast<int>(card.bounds.right - 6 * center.scale_);
                        const int y = static_cast<int>(card.bounds.top + (kMarginForTest + 56 + 55) * center.scale_ - center.scroll_);
                        const auto* pixel = static_cast<const BYTE*>(center.bits_) + (static_cast<size_t>(y) * center.dib_width_ + x) * 4;
                        if (!sample) first_pixel = {pixel[0], pixel[1], pixel[2]};
                        else translucent &= std::abs(first_pixel[0] - pixel[0]) + std::abs(first_pixel[1] - pixel[1]) +
                            std::abs(first_pixel[2] - pixel[2]) > 20;
                    }
                    check(translucent, dark ? "dark card preserves the sampled background through its fill" :
                        "light card preserves the sampled background through its fill");
                    check(SaveDib(center, output / (dark ? L"card-translucent-dark.png" : L"card-translucent-light.png")),
                        "save translucent task card evidence");
                }
            }
            ShowWindow(center.owner_, SW_HIDE);
            check(GetForegroundWindow() == foreground, "offscreen material fixture preserves the user's foreground window");
            center.backdrop_.Reset(); center.backdrop_source_.Reset();
        } else {
            std::printf("[SKIP] acrylic material: Windows transparency disabled or high contrast enabled\n");
        }
        center.open_ = true; center.closing_ = false; center.visibility_ = 0;
        center.animation_from_ = 0; center.animation_start_ = GetTickCount64() - 80;
        center.Tick();
        check(center.AnimationsEnabled() ? center.visibility_ > 0.5f && center.visibility_ < 1 : center.visibility_ == 1,
            "opening uses eased opacity or settles immediately when system motion is disabled");
        center.animation_start_ = GetTickCount64() - 200; center.Tick();
        check(center.visibility_ == 1 && center.open_, "opening settles fully visible");
        center.closing_ = true; center.animation_from_ = 1; center.animation_start_ = GetTickCount64() - 50;
        center.Tick();
        check(center.AnimationsEnabled() ? center.visibility_ > 0.5f && center.visibility_ < 1 : !center.open_,
            "closing uses eased opacity or dismisses immediately when system motion is disabled");
        center.animation_start_ = GetTickCount64() - 200; center.Tick();
        check(!center.open_ && !IsWindowVisible(center.hwnd_), "closing completes without a residual popup");
        center.open_ = true; center.closing_ = true; center.visibility_ = 0.4f;
        // Simulate rapid reversal without showing a foreground window.
        center.closing_ = false; center.animation_from_ = center.visibility_;
        center.animation_start_ = GetTickCount64() - 80; center.Tick();
        check(center.AnimationsEnabled() ? center.visibility_ > 0.4f && center.visibility_ < 1 : center.visibility_ == 1,
            "rapid reversal preserves continuous opacity or respects disabled motion");
        SendMessageW(center.hwnd_, WM_CANCELMODE, 0, 0);
        check(!center.open_, "cancel mode dismisses synchronously");
        center.open_ = true; center.closing_ = false; center.visibility_ = 1;
        SendMessageW(center.hwnd_, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(center.owner_));
        check(center.AnimationsEnabled() ? center.open_ && center.closing_ : !center.open_,
            "returning activation to owner starts the closing animation");
        center.origin_click_tick_ = GetTickCount64();
        center.Toggle(RECT{0, 0, 40, 40});
        check((!center.open_ || center.closing_) && !center.origin_click_tick_,
            "origin click consumed after deactivation keeps closing without reopening or skipping the animation");
        center.open_ = true;
        SendMessageW(center.hwnd_, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!center.open_ || center.closing_, "Escape dismisses through the popup message handler");
        center.Hide(false);
        auto motion_labels = center.labels_;
        motion_labels.title = L"操作中心"; motion_labels.recent = L"近期结果";
        motion_labels.clear_finished = L"清除已结束"; motion_labels.open_destination = L"打开目标文件夹";
        center.SetLabels(std::move(motion_labels));
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            center.motion_override_ = 1;
            center.scale_ = scale; center.painter_.SetScale(scale); center.compositor_->RecreateTextFormats(scale);
            center.SetTheme(dark, HexColor(0x0078D4));
            auto removed = completed, survivor = completed;
            removed.task_id = 202; survivor.task_id = 201;
            removed.summary = L"复制旅行照片"; survivor.summary = L"复制项目资料";
            removed.source_label = L"E:\\照片\\旅行照片"; removed.destination_label = L"D:\\备份\\照片";
            survivor.source_label = L"E:\\项目\\资料"; survivor.destination_label = L"D:\\备份\\资料";
            removed.completed_items = removed.total_items; survivor.completed_items = survivor.total_items;
            center.Update({removed, survivor});
            center.width_ = static_cast<int>(496 * scale); center.height_ = static_cast<int>(624 * scale);
            center.Layout(); center.UpdateHeight(false);
            center.open_ = true; center.closing_ = false; center.visibility_ = 1;
            center.animation_from_ = 1; center.animation_start_ = GetTickCount64() - 1000;
            center.force_solid_ = true;
            check(center.Render(), "render two real cards before removal");
            const float previous_top = center.cards_.back().bounds.top;
            const auto removed_button = std::find_if(center.buttons_.begin(), center.buttons_.end(), [&](const auto& button) {
                return button.task_id == removed.task_id;
            });
            if (removed_button != center.buttons_.end())
                center.pressed_ = center.focused_ = static_cast<int>(removed_button - center.buttons_.begin());
            center.Update({survivor});
            check(center.departing_cards_.size() == 1 && center.cards_animating_ && center.tasks_.size() == 1 &&
                center.pressed_ == -1 && center.focused_ == -1 &&
                std::none_of(center.buttons_.begin(), center.buttons_.end(), [&](const auto& button) { return button.task_id == removed.task_id; }),
                "removed card retains only its image and loses all actions and held input immediately");
            check(std::abs(center.cards_.front().bounds.top - previous_top) < 0.01f,
                "surviving card starts at its displayed position instead of jumping to the new layout");
            const auto started = center.card_animation_start_;
            survivor.percent = 75; center.Update({survivor});
            check(center.card_animation_start_ == started, "ordinary task updates do not restart card removal");
            const auto prefix = L"remove-" + std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark-" : L"-light-");
            float midway_top = previous_top;
            bool unclipped = true;
            for (int frame = 0; frame <= 14; ++frame) {
                const auto elapsed = static_cast<ULONGLONG>(frame * 16);
                center.card_animation_start_ = GetTickCount64() - elapsed;
                center.resize_start_ = GetTickCount64() - elapsed;
                center.Tick();
                if (frame == 7) midway_top = center.cards_.front().bounds.top;
                for (const auto& button : center.buttons_) if (button.action != OperationCenter::Action::Clear)
                    unclipped &= button.bounds.top >= (kMarginForTest + 56) * scale &&
                        button.bounds.bottom <= center.height_ - kMarginForTest * scale;
                if (!SaveDib(center, output / (prefix + std::to_wstring(frame) + L".png"))) ++failures;
            }
            check(midway_top < previous_top && midway_top > center.cards_.front().bounds.top &&
                !center.cards_animating_ && center.departing_cards_.empty() && !center.resizing_,
                "removal fades through intermediate frames and the remaining card and panel settle together");
            check(unclipped && center.cards_.front().section_top == 28 * scale,
                "remaining buttons stay fully visible and the shared section heading does not jump during removal");
            bool hits_follow = true;
            for (size_t i = 0; i < center.buttons_.size(); ++i) {
                const auto& rect = center.buttons_[i].bounds;
                hits_follow &= center.Hit((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f) == static_cast<int>(i);
            }
            check(hits_follow, "live card actions follow the animated layout");
            center.Update({});
            center.card_animation_start_ = GetTickCount64() - 80; center.Tick();
            center.Update({survivor});
            check(std::none_of(center.departing_cards_.begin(), center.departing_cards_.end(), [&](const auto& card) {
                return card.task_id == survivor.task_id;
            }), "a task that returns during removal cannot retain a duplicate fading image");
            center.motion_override_ = 0;
            center.Update({}); center.Tick();
            check(center.departing_cards_.empty() && !center.cards_animating_ && !center.resizing_ && center.buttons_.empty(),
                "disabled motion removes cards and settles panel height immediately");
            center.Hide(false);
        }
        center.motion_override_ = 1;
        center.open_ = true; center.visibility_ = 0; center.BeginVisibility(false);
        center.animation_start_ = GetTickCount64() - 60; center.Tick();
        const float opening = center.visibility_;
        center.Hide();
        check(center.closing_ && center.animation_from_ == opening && center.Hit(40, 40) == -1,
            "closing mid-open continues from the current opacity and immediately disables interaction");
        center.animation_start_ = GetTickCount64() - 50; center.Tick();
        const float closing = center.visibility_;
        center.BeginVisibility(false);
        check(!center.closing_ && center.animation_from_ == closing,
            "reopening a closing panel starts from the currently displayed opacity");
        center.motion_override_ = 0; center.Tick();
        check(center.visibility_ == 1 && !center.closing_, "disabling motion during opening settles the popup immediately");
        center.Hide();
        check(!center.open_, "disabled motion also closes the popup immediately");
        center.motion_override_ = -1; center.force_solid_ = false;
        center.scale_ = 1; center.painter_.SetScale(1); center.compositor_->RecreateTextFormats(1);
        {
            using I = l10n::StringId;
            l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
            OperationCenterLabels localized;
            localized.title = l10n::Get(I::OperationCenter);
            localized.current = l10n::Get(I::OperationCenterCurrent);
            localized.recent = l10n::Get(I::OperationCenterRecent);
            localized.clear_finished = l10n::Get(I::OperationCenterClear);
            localized.details = l10n::Get(I::OpDetails);
            localized.pause = l10n::Get(I::OpPause);
            localized.resume = l10n::Get(I::OpResume);
            localized.cancel = l10n::Get(I::Cancel);
            localized.open_destination = l10n::Get(I::OperationCenterDestination);
            localized.remaining_time = l10n::Get(I::OpRemainingTime);
            localized.remaining_items = l10n::Get(I::OpRemainingItems);
            localized.current_speed = l10n::Get(I::OpCurrentSpeed);
            localized.phase_names[static_cast<size_t>(ops::OpPhase::Running)] = l10n::Get(I::OperationCenterRunning);
            localized.phase_names[static_cast<size_t>(ops::OpPhase::Completed)] = l10n::Get(I::OpCompleted);
            center.SetLabels(std::move(localized));
            auto preview = running;
            preview.type = ops::OpType::Copy;
            preview.summary = L"复制 21 个项目";
            preview.source_label = L"E:\\照片\\旅行照片";
            preview.destination_label = L"D:\\备份\\2026";
            preview.current_item = L"E:\\照片\\旅行照片\\IMG_20261022.jpg";
            auto finished = preview;
            finished.task_id = 30; finished.active = false; finished.phase = ops::OpPhase::Completed;
            finished.percent = 100; finished.completed_items = finished.total_items;
            for (const bool dark : {false, true}) {
                center.SetTheme(dark, HexColor(0x0078D4));
                center.Update({preview, finished}); center.scroll_ = 0;
                check(center.SaveDebugSnapshot((output / (dark ? L"design-zh-dark.png" : L"design-zh-light.png")).c_str(), 472, 600),
                    "localized active and completed cards render with production Chinese labels");
            }
        }
        std::printf("Operation center UI: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    static constexpr float kMarginForTest = 12;
};
} // namespace pulse::ui

int RunOperationCenterUiTest() {
    using namespace pulse::ui;
    const auto output = std::filesystem::absolute(L"bench_data/operation-center-ui");
    std::filesystem::create_directories(output);
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Operation center isolated fixture",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 960, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return 2;
    int result = 2;
    {
        Compositor compositor;
        if (compositor.Init(owner)) {
            compositor.RecreateTextFormats(1);
            OperationCenter center;
            if (center.Create(owner, &compositor)) result = OperationCenterTestPeer::Run(center, output);
        }
    }
    DestroyWindow(owner);
    return result;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = RunOperationCenterUiTest();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
