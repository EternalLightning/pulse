import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import subprocess
import threading
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent
user = C.WinDLL('user32', use_last_error=True)
gdi = C.WinDLL('gdi32', use_last_error=True)
dwm = C.WinDLL('dwmapi')
PTR = C.c_ssize_t
CALLBACK = C.WINFUNCTYPE(W.BOOL, W.HWND, PTR)


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, args
    return fn


enum_windows = bind(user, 'EnumWindows', W.BOOL, CALLBACK, PTR)
enum_children = bind(user, 'EnumChildWindows', W.BOOL, W.HWND, CALLBACK, PTR)
get_pid = bind(user, 'GetWindowThreadProcessId', W.DWORD, W.HWND, C.POINTER(W.DWORD))
visible = bind(user, 'IsWindowVisible', W.BOOL, W.HWND)
get_class = bind(user, 'GetClassNameW', C.c_int, W.HWND, W.LPWSTR, C.c_int)
send = bind(user, 'SendMessageW', PTR, W.HWND, W.UINT, C.c_size_t, PTR)
post = bind(user, 'PostMessageW', W.BOOL, W.HWND, W.UINT, C.c_size_t, PTR)
set_text = bind(user, 'SetWindowTextW', W.BOOL, W.HWND, W.LPCWSTR)
window_rect = bind(user, 'GetWindowRect', W.BOOL, W.HWND, C.POINTER(W.RECT))
client_rect = bind(user, 'GetClientRect', W.BOOL, W.HWND, C.POINTER(W.RECT))
set_pos = bind(user, 'SetWindowPos', W.BOOL, W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT)
foreground = bind(user, 'GetForegroundWindow', W.HWND)
activate = bind(user, 'SetForegroundWindow', W.BOOL, W.HWND)
invalidate = bind(user, 'InvalidateRect', W.BOOL, W.HWND, C.POINTER(W.RECT), W.BOOL)
get_dc = bind(user, 'GetDC', W.HDC, W.HWND)
release_dc = bind(user, 'ReleaseDC', C.c_int, W.HWND, W.HDC)
create_dc = bind(gdi, 'CreateCompatibleDC', W.HDC, W.HDC)
select_object = bind(gdi, 'SelectObject', W.HANDLE, W.HDC, W.HANDLE)
delete_object = bind(gdi, 'DeleteObject', W.BOOL, W.HANDLE)
delete_dc = bind(gdi, 'DeleteDC', W.BOOL, W.HDC)
bitblt = bind(gdi, 'BitBlt', W.BOOL, W.HDC, C.c_int, C.c_int, C.c_int, C.c_int, W.HDC, C.c_int, C.c_int, W.DWORD)


class Header(C.Structure):
    _fields_ = [('size', W.DWORD), ('width', W.LONG), ('height', W.LONG),
                ('planes', W.WORD), ('bits', W.WORD), ('compression', W.DWORD),
                ('image_size', W.DWORD), ('xpels', W.LONG), ('ypels', W.LONG),
                ('used', W.DWORD), ('important', W.DWORD)]


create_dib = bind(gdi, 'CreateDIBSection', W.HBITMAP, W.HDC, C.POINTER(Header), W.UINT,
                  C.POINTER(C.c_void_p), W.HANDLE, W.DWORD)


def windows(pid, parent=None):
    found = []

    @CALLBACK
    def visit(hwnd, _):
        process = W.DWORD()
        get_pid(hwnd, C.byref(process))
        if process.value == pid and visible(hwnd):
            name = C.create_unicode_buffer(256)
            get_class(hwnd, name, len(name))
            found.append((hwnd, name.value))
        return True

    if parent:
        enum_children(parent, visit, 0)
    else:
        enum_windows(visit, 0)
    return found


def capture(hwnd):
    rect = W.RECT()
    window_rect(hwnd, C.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    source = get_dc(None)
    target = create_dc(source)
    header = Header(C.sizeof(Header), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
    bits = C.c_void_p()
    dib = create_dib(target, C.byref(header), 0, C.byref(bits), None, 0)
    if not dib:
        raise C.WinError(C.get_last_error())
    old = select_object(target, dib)
    try:
        dwm.DwmFlush()
        if not bitblt(target, 0, 0, width, height, source, rect.left, rect.top, 0x40CC0020):
            raise C.WinError(C.get_last_error())
        gdi.GdiFlush()
        return np.frombuffer(C.string_at(bits, width * height * 4), np.uint8).reshape(height, width, 4).copy()
    finally:
        select_object(target, old)
        delete_object(dib)
        delete_dc(target)
        release_dc(None, source)


def save(pixels, name):
    Image.fromarray(pixels[:, :, [2, 1, 0]]).save(ROOT / name)


def difference(a, b):
    return int(np.count_nonzero(np.max(np.abs(a[:, :, :3].astype(np.int16) - b[:, :, :3]), axis=2) > 1))


def run(executable, label, mode, kind, text_value='Pulse_中文_123.txt', caret=3, theme=2):
    fixture = ROOT / 'files'
    fixture.mkdir(exist_ok=True)
    (fixture / 'Pulse_中文_123.txt').touch()
    profile = ROOT / ('profile-' + label)
    profile.mkdir(exist_ok=True)
    (profile / 'app.json').write_text(json.dumps({'text_render': mode, 'language': 'zh-CN',
                                                'theme_mode': theme, 'start_to_tray': False}), encoding='utf-8')
    env = os.environ.copy()
    env['PULSE_TEST_DATA_DIR'] = str(profile)
    process = subprocess.Popen([str(executable), '--test-instance', str(fixture)], cwd=executable.parent, env=env)
    main = None
    try:
        for _ in range(160):
            candidates = [(hwnd, name) for hwnd, name in windows(process.pid) if 'Pulse' in name]
            if candidates:
                main = candidates[0][0]
                break
            if process.poll() is not None:
                raise RuntimeError(f'{label} exited early: {process.returncode}')
            time.sleep(.05)
        if not main:
            raise RuntimeError(f'{label}: no main window')
        set_pos(main, W.HWND(-1), 40, 40, 1100, 720, 0x40)
        activate(main)
        time.sleep(.8)
        send(main, 0x100, 0x73 if kind == 'address' else 0x71, 0)
        time.sleep(.4)
        edits = [(hwnd, name) for hwnd, name in windows(process.pid, main) if name.casefold() == 'edit']
        if len(edits) != 1:
            save(capture(main), f'{label}-{kind}-no-editor.png')
            raise RuntimeError(f'{label}: expected one editor, got {edits}')
        edit = edits[0][0]
        text = C.create_unicode_buffer(text_value)
        if not send(edit, 0xC, 0, C.cast(text, C.c_void_p).value):
            raise RuntimeError('WM_SETTEXT failed')
        send(edit, 0xB1, caret, caret)
        time.sleep(.1)
        left = capture(edit)
        send(edit, 0x100, 0x27, 0)
        right = capture(edit)
        save(left, f'{label}-{kind}-left.png')
        save(right, f'{label}-{kind}-right.png')
        save(capture(main), f'{label}-{kind}-window.png')
        result = {'label': label, 'kind': kind, 'pid': process.pid, 'samples': 0, 'unexpected': 0,
                  'worst_difference': 0, 'width': left.shape[1], 'height': left.shape[0]}
        stop = threading.Event()
        errors = []

        def sample():
            try:
                while not stop.is_set():
                    pixels = capture(edit)
                    value = min(difference(pixels, left), difference(pixels, right))
                    result['samples'] += 1
                    result['unexpected'] += int(value > left.shape[0] * 4)
                    if value > result['worst_difference']:
                        result['worst_difference'] = value
                        save(pixels, f'{label}-{kind}-worst.png')
            except BaseException as exc:
                errors.append(repr(exc))

        sampler = threading.Thread(target=sample)
        sampler.start()
        for index in range(120):
            send(edit, 0x100, 0x25 if index % 2 == 0 else 0x27, 0)
            invalidate(main, None, False)
            time.sleep(.012)
        stop.set()
        sampler.join()
        if errors:
            raise RuntimeError(errors)
        send(edit, 0x100, 0x1B, 0)
        print(json.dumps(result), flush=True)
        (ROOT / f'{label}-{kind}-result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        return result
    finally:
        if main:
            post(main, 0x10, 0, 0)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('label')
    parser.add_argument('--mode', type=int, default=0)
    parser.add_argument('--kind', choices=['address', 'rename'], default='address')
    args = parser.parse_args()
    user.SetProcessDpiAwarenessContext(C.c_void_p(-4))
    previous = foreground()
    try:
        run(args.executable.resolve(), args.label, args.mode, args.kind)
    finally:
        if previous:
            activate(previous)
