// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Included inside OperationCenterTestPeer.
static int RunSmoothScroll(OperationCenter& center, const std::filesystem::path& output) {
    int failures=0,checks=0;
    const auto check=[&](bool ok,const char* label) {
        ++checks;failures+=!ok;
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
    };
    std::vector<ops::OpStatus> tasks;
    for(uint64_t id=1;id<=18;++id) {
        ops::OpStatus task;
        task.task_id=id;task.type=ops::OpType::Copy;task.phase=ops::OpPhase::Running;
        task.active=true;task.percent=25;task.total_items=100;task.completed_items=25;
        task.source_label=L"C:\\Fixture\\source-"+std::to_wstring(id);
        task.destination_label=L"D:\\Fixture\\target-"+std::to_wstring(id);
        tasks.push_back(std::move(task));
    }
    int pause_calls=0;
    OperationCenterCallbacks callbacks;
    callbacks.pause=[&](uint64_t) {++pause_calls;};
    callbacks.cancel=[](uint64_t) {};
    center.SetCallbacks(std::move(callbacks));
    center.force_solid_=true;
    center.motion_override_=1;
    for(float scale:{1.0f,1.5f,2.0f}) for(bool dark:{false,true}) {
        center.Hide(false);
        center.scale_=scale;center.painter_.SetScale(scale);
        center.compositor_->RecreateTextFormats(scale);
        center.SetTheme(dark,HexColor(0x0078D4));
        center.Update(tasks);
        center.width_=static_cast<int>(496*scale);center.height_=static_cast<int>(440*scale);
        center.target_height_=center.height_;center.resizing_=false;
        center.work_area_={0,0,static_cast<LONG>(1800*scale),static_cast<LONG>(1800*scale)};
        center.open_=true;center.visibility_=center.animation_from_=1;
        center.animation_start_=AnimationTimeMs()-1000;
        center.Layout();
        const auto maximum=[&] {return (std::max)(0.0f,center.content_height_-center.body_height_);};
        const auto reset=[&] {
            center.Hide(false);
            center.motion_override_=1;center.open_=true;center.closing_=false;
            center.FinishCardTransition();center.resizing_=false;
            center.StopScroll();center.Scroll(-1e6f);
            MSG message{};
            while (PeekMessageW(&message, center.hwnd_, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        };
        const auto pending = [&] { return center.scroll_motion_.Pending(); };
        const auto target = [&] {
            auto motion = center.scroll_motion_;
            float value = center.scroll_;
            motion.Tick(value, maximum(), AnimationTimeMs(), false);
            return value;
        };
        const auto advance = [&](uint64_t elapsed) { center.TickScroll(AnimationTimeMs() + elapsed); center.Redraw(); };
        const auto wheel=[&](short delta) {
            SendMessageW(center.hwnd_,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),0);
        };
        check(maximum()>72*scale,"fixture has enough cards to exercise real scrolling");
        reset();
        const auto wheel_started=AnimationTimeMs();
        wheel(-WHEEL_DELTA);
        check(center.scroll_==0 && pending() && std::abs(target()-72*scale)<0.1f,
            "native wheel starts easing instead of jumping the visible offset");
        center.TickScroll(wheel_started+16);center.Redraw();
        check(center.scroll_ > 8*scale && center.scroll_ < 14*scale,
            "first 16 ms follows shared responsive scrolling instead of slow acceleration");
        check(center.scroll_>0 && center.scroll_<72*scale,"a real animation tick displays an intermediate wheel offset");
        const float first=center.scroll_;
        const float previous_target=target();
        wheel(-WHEEL_DELTA);
        check(std::abs(target()-previous_target-72*scale)<0.1f && center.scroll_<target(),
            "successive wheel input accumulates distance without snapping to the target");
        const float before_continue=center.scroll_;
        advance(16);
        check(center.scroll_-before_continue > first,
            "successive wheel input retains momentum instead of restarting acceleration");
        const float before_reverse=center.scroll_;
        wheel(WHEEL_DELTA);
        check(target()<before_reverse,"reverse wheel targets the opposite direction from the displayed offset");
        check(std::abs(target()-(std::max)(0.0f,center.scroll_-72*scale))<0.1f,
            "reverse wheel discards forward momentum before moving in the new direction");
        advance(16);
        check(center.scroll_<before_reverse,"reverse wheel changes direction on the next frame");
        check(first>0,"forward input advanced before reversing");

        reset();for(int i=0;i<4;++i) wheel(-WHEEL_DELTA/4);
        check(std::abs(target()-72*scale)<0.1f,"high-resolution wheel deltas accumulate without truncation");
        reset();center.SetScrollOffset(maximum()-20*scale);wheel(-32760);
        check(target()==maximum(),"wheel target clamps to the final card");
        advance(2000);
        check(!pending() && center.scroll_==maximum(),"bottom edge settles exactly and stops scrolling");
        wheel(-WHEEL_DELTA);
        check(!pending(),"additional outward wheel input does not restart edge animation");
        center.SetScrollOffset(20*scale);wheel(32760);advance(2000);
        check(!pending() && center.scroll_==0,"reverse edge input settles at the start");

        reset();wheel(-WHEEL_DELTA);
        const int bar_x=static_cast<int>(center.width_-10*scale);
        const int bar_y=static_cast<int>((12+56)*scale+center.body_height_*0.5f);
        SendMessageW(center.hwnd_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(bar_x,bar_y));
        const float dragged=center.scroll_;
        check(center.dragging_scrollbar_ && !pending() && dragged>0,
            "scrollbar press cancels easing and immediately follows the pointer");
        SendMessageW(center.hwnd_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bar_x,bar_y+static_cast<int>(20*scale)));
        check(center.scroll_>dragged && !pending(),"scrollbar dragging stays direct during a wheel interruption");
        SendMessageW(center.hwnd_,WM_LBUTTONUP,0,MAKELPARAM(bar_x,bar_y));

        reset();wheel(-WHEEL_DELTA);
        const auto action=std::find_if(center.buttons_.begin(),center.buttons_.end(),[&](const auto& button) {
            return button.action==OperationCenter::Action::Pause &&
                button.bounds.top>=(12+56)*scale && button.bounds.bottom<=center.height_-12*scale;
        });
        check(action!=center.buttons_.end(),"scrolling fixture exposes a visible production task action");
        if(action!=center.buttons_.end()) {
            const auto bounds=action->bounds;
            const auto point=MAKELPARAM(static_cast<int>((bounds.left+bounds.right)*0.5f),
                static_cast<int>((bounds.top+bounds.bottom)*0.5f));
            const int before=pause_calls;
            SendMessageW(center.hwnd_,WM_LBUTTONDOWN,MK_LBUTTON,point);
            check(!pending() && center.pressed_>=0,"pressing a live task action freezes the scrolling viewport");
            SendMessageW(center.hwnd_,WM_LBUTTONUP,0,point);
            check(pause_calls==before+1,"release invokes the pressed task instead of a moving neighbour");
        }
        reset();wheel(-WHEEL_DELTA);
        center.focused_=-1;center.FocusButton(-1);
        check(!pending() && center.focused_>=0 && center.scroll_>0,
            "keyboard focus cancels wheel motion and reveals the focused action immediately");
        reset();SendMessageW(center.hwnd_,WM_KEYDOWN,VK_NEXT,0);
        check(pending() && center.scroll_==0,"Page Down uses the same smooth path");
        SendMessageW(center.hwnd_,WM_KEYDOWN,VK_HOME,0);
        check(!pending() && center.scroll_==0,"Home cancels pending scrolling and jumps to the first card");

        reset();
        const auto before_update = center.buttons_;
        center.scroll_motion_.ScrollBy(center.scroll_, 72 * scale, maximum(), center.body_height_,
            AnimationTimeMs() - 40, true);
        const auto saved_target=target();
        center.Update(tasks);
        check(std::abs(target()-saved_target)<0.1f && pending(),
            "ordinary task updates preserve the running scroll clock and target");
        check(center.scroll_ > 0, "task refresh advances an in-flight scroll while rebuilding action positions");
        bool aligned = center.buttons_.size() == before_update.size();
        int visible_actions = 0;
        for (size_t i = 0; i < center.buttons_.size() && i < before_update.size(); ++i) {
            const auto& button = center.buttons_[i];
            const float offset = (12 + 56) * scale - center.scroll_;
            aligned &= std::abs(button.content_top - before_update[i].content_top) < 0.1f &&
                std::abs(button.bounds.top - before_update[i].content_top - offset) < 0.1f &&
                std::abs(button.bounds.bottom - before_update[i].content_bottom - offset) < 0.1f;
            if (button.bounds.top >= (12 + 56) * scale && button.bounds.bottom <= center.height_ - 12 * scale) {
                ++visible_actions;
                aligned &= center.Hit((button.bounds.left + button.bounds.right) * 0.5f,
                    (button.bounds.top + button.bounds.bottom) * 0.5f) == static_cast<int>(i);
            }
        }
        check(aligned && visible_actions > 0, "task actions stay on their cards and remain clickable after a scrolling refresh");
        check(SaveDib(center, output / (L"scroll-refresh-" + std::to_wstring(static_cast<int>(scale * 100)) +
            (dark ? L"-dark.png" : L"-light.png"))), "capture action visibility after a scrolling refresh");
        center.motion_override_=0;center.Tick();
        check(!pending() && std::abs(center.scroll_-saved_target)<0.1f,"reduced motion settles an active scroll at its target");
        reset();center.motion_override_=0;wheel(-WHEEL_DELTA);
        check(!pending() && center.scroll_==72*scale,"disabled animations retain immediate wheel scrolling");

        reset();wheel(-WHEEL_DELTA);center.Hide();
        check(center.closing_ && !pending(),"closing the popup cancels pending wheel animation");
        const float closing_offset=center.scroll_;
        wheel(-WHEEL_DELTA);center.Tick();
        check(center.scroll_==closing_offset && !pending(),"a closing popup does not accept more wheel motion");
        center.Hide(false);

        // Observe production frame messages, including rendering and layered-window presentation.
        reset();center.Render(false);wheel(-WHEEL_DELTA);
        const auto deadline=AnimationTimeMs()+1200;
        int intermediate=0,frames=0;bool monotonic=true;
        using Clock=std::chrono::steady_clock;
        auto last_frame=Clock::now();
        double total_interval=0,worst_interval=0,total_dispatch=0;
        float previous=0;
        while(pending() && AnimationTimeMs()<deadline) {
            MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            MSG message{};
            while(PeekMessageW(&message,center.hwnd_,0,0,PM_REMOVE)) {
                const auto before=Clock::now();
                DispatchMessageW(&message);
                if(message.message==EditMotionFrameMessage() || message.message==WM_TIMER) {
                    const auto after=Clock::now();
                    const double interval=std::chrono::duration<double,std::milli>(after-last_frame).count();
                    total_interval+=interval;worst_interval=(std::max)(worst_interval,interval);
                    total_dispatch+=std::chrono::duration<double,std::milli>(after-before).count();
                    last_frame=after;
                    ++frames;intermediate+=center.scroll_>0 && center.scroll_<72*scale;
                    monotonic&=center.scroll_>=previous;previous=center.scroll_;
                }
            }
        }
        check(monotonic && intermediate>=2 && !pending() && std::abs(center.scroll_-72*scale)<0.1f,
            "production frame scheduler presents intermediate frames and settles without indeterminate tasks");
        std::printf("[FRAMES] dpi=%.0f dark=%d dispatched=%d intermediate=%d\n",scale*100,dark,frames,intermediate);
        if(frames) std::printf("[TIMING] dpi=%.0f dark=%d interval_avg=%.2fms interval_max=%.2fms dispatch_avg=%.2fms\n",
            scale*100,dark,total_interval/frames,worst_interval,total_dispatch/frames);

        reset();wheel(-WHEEL_DELTA);
        const std::wstring prefix=L"scroll-"+std::to_wstring(static_cast<int>(scale*100))+(dark?L"-dark-":L"-light-");
        const auto capture_started=AnimationTimeMs();
        for(int elapsed:{0,40,80,600}) {
            center.TickScroll(capture_started+elapsed);center.Redraw();
            check(SaveDib(center,output/(prefix+std::to_wstring(elapsed)+L".png")),"capture production scroll animation frame");
        }
        wheel(-WHEEL_DELTA);center.Update({});
        check(!pending() && center.scroll_==0 && target()==0,
            "clearing tasks cancels pending scrolling and clamps the empty viewport");
    }
    center.Hide(false);center.motion_override_=-1;center.force_solid_=false;
    std::printf("[SUMMARY] Operation center smooth scroll: %d PASS / %d FAIL\n",checks-failures,failures);
    return failures?1:0;
}

static int RunScrollCadence(OperationCenter& center) {
    using Clock = std::chrono::steady_clock;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    SetWindowPos(center.owner_, nullptr, 20, 20, 1100, 1320,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    center.compositor_->Resize(1600, 1600);
    auto* dc = center.compositor_->Dc();
    dc->BeginDraw(); dc->Clear(HexColor(0x789ABC));
    check(SUCCEEDED(dc->EndDraw()), "prepare visible owner for real acrylic sampling");
    center.compositor_->Present();
    center.motion_override_ = 1;
    center.SetCallbacks({});
    for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
        center.Hide(false);
        center.scale_ = scale; center.painter_.SetScale(scale);
        center.compositor_->RecreateTextFormats(scale);
        center.SetTheme(dark, HexColor(0x0078D4));
        std::vector<ops::OpStatus> tasks;
        for (uint64_t id = 1; id <= 18; ++id) {
            ops::OpStatus task;
            task.task_id = id; task.type = ops::OpType::Copy; task.phase = ops::OpPhase::Running;
            task.active = true; task.percent = 25;
            task.source_label = L"C:\\Fixture\\source-" + std::to_wstring(id);
            task.destination_label = L"D:\\Fixture\\target-" + std::to_wstring(id);
            tasks.push_back(std::move(task));
        }
        center.Update(tasks);
        center.width_ = static_cast<int>(496 * scale); center.height_ = static_cast<int>(624 * scale);
        center.target_height_ = center.height_; center.resizing_ = false;
        center.work_area_ = {0, 0, 4000, 4000}; center.base_x_ = center.base_y_ = 30;
        center.open_ = true; center.closing_ = false; center.visibility_ = center.animation_from_ = 1;
        center.animation_start_ = AnimationTimeMs() - 1000;
        center.scroll_ = 0; center.StopScroll();
        center.force_solid_ = false; center.Layout();
        check(center.Render() && center.acrylic_, "real operation popup prepares its acrylic material");
        center.Present(); ShowWindow(center.hwnd_, SW_SHOWNOACTIVATE);
        auto* const plate = center.backdrop_.plate_.get();
        bool material_stable = true, monotonic = true;
        int frames = 0, timers = 0, updates = 0;
        double dispatch_ms = 0;
        float previous = center.scroll_;
        std::vector<double> intervals;
        auto last_frame = Clock::now();
        const auto started = AnimationTimeMs();
        auto next_wheel = started + 40;
        SendMessageW(center.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        while (center.scroll_motion_.Pending() && AnimationTimeMs() < started + 2000) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                const auto before = Clock::now();
                DispatchMessageW(&message);
                if (message.hwnd == center.hwnd_ &&
                    (message.message == EditMotionFrameMessage() || message.message == WM_TIMER)) {
                    const auto after = Clock::now();
                    if (frames >= 3) intervals.push_back(std::chrono::duration<double, std::milli>(after - last_frame).count());
                    dispatch_ms += std::chrono::duration<double, std::milli>(after - before).count();
                    last_frame = after; ++frames; timers += message.message == WM_TIMER;
                    monotonic &= center.scroll_ >= previous; previous = center.scroll_;
                    material_stable &= center.acrylic_ && center.backdrop_.plate_.get() == plate;
                    if (frames % 4 == 0 && AnimationTimeMs() - started < 300) {
                        tasks.front().percent += 1;
                        center.Update(tasks); ++updates;
                    }
                }
            }
            const auto now = AnimationTimeMs();
            if (now >= next_wheel && now - started < 300) {
                SendMessageW(center.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
                next_wheel += 40;
            }
        }
        MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        const bool queried = GetMonitorInfoW(MonitorFromWindow(center.hwnd_, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode);
        const DWORD refresh = queried && mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 60;
        double total = 0;
        for (double interval : intervals) total += interval;
        std::sort(intervals.begin(), intervals.end());
        const double average = intervals.empty() ? 0 : total / static_cast<double>(intervals.size());
        const double p95 = intervals.empty() ? 0 : intervals[(intervals.size() - 1) * 95 / 100];
        const double worst = intervals.empty() ? 0 : intervals.back();
        check(frames >= 10 && updates > 0 && monotonic && !center.scroll_motion_.Pending(),
            "continuous wheel input settles while real task updates keep arriving");
        check(material_stable, "visible acrylic plate stays cached throughout scrolling and task updates");
        check(frames > 0 && timers == 0, "scroll animation uses monitor-paced frames instead of window timers");
        check(!intervals.empty() && average <= 1800.0 / refresh,
            "scroll frame cadence tracks the monitor refresh rate");
        std::printf("[CADENCE] dpi=%.0f dark=%d hz=%lu frames=%d updates=%d avg=%.2fms p95=%.2fms max=%.2fms dispatch=%.2fms\n",
            scale * 100, dark, static_cast<unsigned long>(refresh), frames, updates, average, p95, worst,
            frames ? dispatch_ms / frames : 0);
        double render_ms = 0, present_ms = 0;
        for (int frame = 0; frame < 32; ++frame) {
            center.SetScrollOffset(center.scroll_ + 2.25f * scale);
            const auto before_render = Clock::now();
            center.Render(false);
            const auto before_present = Clock::now();
            center.Present();
            const auto after_present = Clock::now();
            render_ms += std::chrono::duration<double, std::milli>(before_present - before_render).count();
            present_ms += std::chrono::duration<double, std::milli>(after_present - before_present).count();
        }
        std::printf("[PHASE] dpi=%.0f dark=%d render=%.2fms present=%.2fms\n", scale * 100, dark, render_ms / 32, present_ms / 32);
        WPARAM stale_token = 0;
        SendMessageW(center.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        MSG pending{};
        const auto deadline = AnimationTimeMs() + 100;
        while (!stale_token && AnimationTimeMs() < deadline) {
            if (PeekMessageW(&pending, center.hwnd_, EditMotionFrameMessage(), EditMotionFrameMessage(), PM_REMOVE))
                stale_token = pending.wParam;
            else MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        center.Hide(false);
        const float hidden_offset = center.scroll_;
        SendMessageW(center.hwnd_, EditMotionFrameMessage(), stale_token, 0);
        check(stale_token && !center.open_ && center.scroll_ == hidden_offset && !center.scroll_motion_.Pending(),
            "closing cancels the frame wait and rejects an already queued animation frame");
    }
    center.motion_override_ = -1;
    ShowWindow(center.owner_, SW_HIDE);
    std::printf("[SUMMARY] Operation center live scroll cadence: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

static int RunScrollPerformance(OperationCenter& center) {
    using Clock=std::chrono::steady_clock;
    int failures=0;
    int queries=0;
    OperationCenterCallbacks callbacks;
    callbacks.can_resume_incomplete=[&](uint64_t) { ++queries;return true; };
    callbacks.resume_incomplete=[](uint64_t) {};
    center.SetCallbacks(std::move(callbacks));
    center.force_solid_=true;
    center.motion_override_=1;
    for(float scale:{1.0f,1.5f,2.0f}) for(bool dark:{false,true}) for(int count:{18,64}) {
        center.Hide(false);
        center.scale_=scale;center.painter_.SetScale(scale);
        center.compositor_->RecreateTextFormats(scale);
        center.SetTheme(dark,HexColor(0x0078D4));
        std::vector<ops::OpStatus> tasks;
        for(int id=1;id<=count;++id) {
            ops::OpStatus task;
            task.task_id=id;task.type=ops::OpType::Copy;task.phase=ops::OpPhase::Completed;
            task.summary=L"复制文件完成";task.percent=100;
            task.source_label=L"C:\\Fixture\\项目资料\\source-"+std::to_wstring(id);
            task.destination_label=L"D:\\Fixture\\归档备份\\target-"+std::to_wstring(id);
            tasks.push_back(std::move(task));
        }
        center.Update(std::move(tasks));
        center.width_=static_cast<int>(496*scale);center.height_=static_cast<int>(440*scale);
        center.target_height_=center.height_;center.resizing_=false;
        center.work_area_={0,0,4000,4000};
        center.open_=true;center.closing_=false;center.visibility_=center.animation_from_=1;
        center.animation_start_=AnimationTimeMs()-1000;
        center.scroll_=0;center.StopScroll();center.Layout();center.Redraw();
        const auto before_cards=center.cards_;
        const auto before_buttons=center.buttons_;
        const int before_queries=queries;
        double total=0,worst=0;
        for(int frame=0;frame<32;++frame) {
            const auto started=Clock::now();
            center.SetScrollOffset((frame+1)*3.25f*scale);center.Redraw();
            const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
            total+=elapsed;worst=(std::max)(worst,elapsed);
        }
        const bool no_layout=queries==before_queries;
        bool geometry=center.cards_.size()==before_cards.size() && center.buttons_.size()==before_buttons.size();
        if(geometry) {
            for(size_t i=0;i<before_cards.size();++i)
                geometry&=center.cards_[i].bounds.top==before_cards[i].bounds.top;
            for(size_t i=0;i<before_buttons.size();++i)
                geometry&=std::abs(center.buttons_[i].bounds.top-(before_buttons[i].bounds.top-center.scroll_))<0.02f;
        }
        failures+=!no_layout;failures+=!geometry;
        std::printf("[%s] scroll-only frames avoid task action queries (dpi=%.0f dark=%d cards=%d queries=%d)\n",
            no_layout?"PASS":"FAIL",scale*100,dark,count,queries-before_queries);
        std::printf("[%s] content geometry stays fixed and action hit rectangles follow the scroll\n",geometry?"PASS":"FAIL");
        std::printf("[SCROLL COST] dpi=%.0f dark=%d cards=%d frame_avg=%.2fms frame_max=%.2fms\n",
            scale*100,dark,count,total/32,worst);
        const float maximum=center.content_height_-center.body_height_;
        for(int frame=0;frame<2000;++frame)
            center.SetScrollOffset(std::fmod(frame*3.14159f,maximum));
        bool stable=true;
        for(size_t i=0;i<before_buttons.size();++i)
            stable&=std::abs(center.buttons_[i].bounds.top-(before_buttons[i].bounds.top-center.scroll_))<0.02f;
        failures+=!stable;
        std::printf("[%s] repeated fractional scrolling retains precise action hit rectangles\n",stable?"PASS":"FAIL");
    }
    center.Hide(false);center.motion_override_=-1;center.force_solid_=false;
    std::printf("[SUMMARY] Operation center scroll performance: %d failure(s)\n",failures);
    return failures?1:0;
}
