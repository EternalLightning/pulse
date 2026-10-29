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
            center.motion_override_=1;center.open_=true;center.closing_=false;
            center.FinishCardTransition();center.resizing_=false;
            center.StopScroll();center.Scroll(-1e6f);
        };
        const auto wheel=[&](short delta) {
            SendMessageW(center.hwnd_,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),0);
        };
        check(maximum()>72*scale,"fixture has enough cards to exercise real scrolling");
        reset();wheel(-WHEEL_DELTA);
        check(center.scroll_==0 && center.scroll_animating_ && center.scroll_target_==72*scale,
            "native wheel starts easing instead of jumping the visible offset");
        center.scroll_started_=AnimationTimeMs()-40;center.Tick();
        check(center.scroll_>0 && center.scroll_<72*scale,"a real animation tick displays an intermediate wheel offset");
        const float first=center.scroll_;
        const float target=center.scroll_target_;
        wheel(-WHEEL_DELTA);
        check(center.scroll_target_==target+72*scale && center.scroll_<center.scroll_target_,
            "successive wheel input accumulates distance without snapping to the target");
        const float before_reverse=center.scroll_;
        wheel(WHEEL_DELTA);
        check(center.scroll_target_<before_reverse,"reverse wheel targets the opposite direction from the displayed offset");
        center.scroll_started_=AnimationTimeMs()-40;center.Tick();
        check(center.scroll_<before_reverse,"reverse wheel changes direction on the next frame");
        check(first>0,"forward input advanced before reversing");

        reset();for(int i=0;i<4;++i) wheel(-WHEEL_DELTA/4);
        check(std::abs(center.scroll_target_-72*scale)<0.1f,"high-resolution wheel deltas accumulate without truncation");
        reset();wheel(-32760);
        check(center.scroll_target_==maximum(),"wheel target clamps to the final card");
        center.scroll_started_=AnimationTimeMs()-200;center.Tick();
        check(!center.scroll_animating_ && center.scroll_==maximum(),"bottom edge settles exactly and stops scrolling");
        wheel(-WHEEL_DELTA);
        check(!center.scroll_animating_,"additional outward wheel input does not restart edge animation");
        wheel(32760);center.scroll_started_=AnimationTimeMs()-200;center.Tick();
        check(!center.scroll_animating_ && center.scroll_==0,"reverse edge input settles at the start");

        reset();wheel(-WHEEL_DELTA);
        const int bar_x=static_cast<int>(center.width_-10*scale);
        const int bar_y=static_cast<int>((12+56)*scale+center.body_height_*0.5f);
        SendMessageW(center.hwnd_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(bar_x,bar_y));
        const float dragged=center.scroll_;
        check(center.dragging_scrollbar_ && !center.scroll_animating_ && dragged>0,
            "scrollbar press cancels easing and immediately follows the pointer");
        SendMessageW(center.hwnd_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bar_x,bar_y+static_cast<int>(20*scale)));
        check(center.scroll_>dragged && !center.scroll_animating_,"scrollbar dragging stays direct during a wheel interruption");
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
            check(!center.scroll_animating_ && center.pressed_>=0,"pressing a live task action freezes the scrolling viewport");
            SendMessageW(center.hwnd_,WM_LBUTTONUP,0,point);
            check(pause_calls==before+1,"release invokes the pressed task instead of a moving neighbour");
        }
        reset();wheel(-WHEEL_DELTA);
        center.focused_=-1;center.FocusButton(-1);
        check(!center.scroll_animating_ && center.focused_>=0 && center.scroll_>0,
            "keyboard focus cancels wheel motion and reveals the focused action immediately");
        reset();SendMessageW(center.hwnd_,WM_KEYDOWN,VK_NEXT,0);
        check(center.scroll_animating_ && center.scroll_==0,"Page Down uses the same smooth path");
        SendMessageW(center.hwnd_,WM_KEYDOWN,VK_HOME,0);
        check(!center.scroll_animating_ && center.scroll_==0,"Home cancels pending scrolling and jumps to the first card");

        reset();wheel(-WHEEL_DELTA);
        const auto started=center.scroll_started_;
        const auto pending=center.scroll_target_;
        center.Update(tasks);
        check(center.scroll_started_==started && center.scroll_target_==pending && center.scroll_animating_,
            "ordinary task updates preserve the running scroll clock and target");
        center.motion_override_=0;center.Tick();
        check(!center.scroll_animating_ && center.scroll_==pending,"reduced motion settles an active scroll at its target");
        reset();center.motion_override_=0;wheel(-WHEEL_DELTA);
        check(!center.scroll_animating_ && center.scroll_==72*scale,"disabled animations retain immediate wheel scrolling");

        reset();wheel(-WHEEL_DELTA);center.Hide();
        check(center.closing_ && !center.scroll_animating_,"closing the popup cancels pending wheel animation");
        const float closing_offset=center.scroll_;
        wheel(-WHEEL_DELTA);center.Tick();
        check(center.scroll_==closing_offset && !center.scroll_animating_,"a closing popup does not accept more wheel motion");
        center.Hide(false);

        // Observe the popup's own WM_TIMER, including real rendering and presentation.
        reset();center.Render(false);wheel(-WHEEL_DELTA);
        const auto deadline=AnimationTimeMs()+1200;
        int intermediate=0,frames=0;bool monotonic=true;
        float previous=0;
        while(center.scroll_animating_ && AnimationTimeMs()<deadline) {
            MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            MSG message{};
            while(PeekMessageW(&message,center.hwnd_,0,0,PM_REMOVE)) {
                DispatchMessageW(&message);
                if(message.message==WM_TIMER) {
                    ++frames;intermediate+=center.scroll_>0 && center.scroll_<72*scale;
                    monotonic&=center.scroll_>=previous;previous=center.scroll_;
                }
            }
        }
        check(monotonic && intermediate>=2 && !center.scroll_animating_ && center.scroll_==72*scale,
            "production timer presents multiple intermediate frames and settles without indeterminate tasks");
        std::printf("[FRAMES] dpi=%.0f dark=%d dispatched=%d intermediate=%d\n",scale*100,dark,frames,intermediate);

        reset();wheel(-WHEEL_DELTA);
        const std::wstring prefix=L"scroll-"+std::to_wstring(static_cast<int>(scale*100))+(dark?L"-dark-":L"-light-");
        for(int elapsed:{0,40,80,160}) {
            center.scroll_started_=AnimationTimeMs()-elapsed;center.Tick();
            check(SaveDib(center,output/(prefix+std::to_wstring(elapsed)+L".png")),"capture production scroll animation frame");
        }
        wheel(-WHEEL_DELTA);center.Update({});
        check(!center.scroll_animating_ && center.scroll_==0 && center.scroll_target_==0,
            "clearing tasks cancels pending scrolling and clamps the empty viewport");
    }
    center.Hide(false);center.motion_override_=-1;center.force_solid_=false;
    std::printf("[SUMMARY] Operation center smooth scroll: %d PASS / %d FAIL\n",checks-failures,failures);
    return failures?1:0;
}
