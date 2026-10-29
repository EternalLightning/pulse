"""Compare existing edit presentation paths in isolated test processes only."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import argparse
import json

import numpy as np
from PIL import Image

import live_compare as live


parser = argparse.ArgumentParser()
parser.add_argument('executable', type=Path)
parser.add_argument('label')
parser.add_argument('--presentation', choices=['current', 'uploaded', 'native'], required=True)
parser.add_argument('--expect-composed', action='store_true')
parser.add_argument('--mode', type=int, choices=[0, 1, 2], default=1)
parser.add_argument('--kind', choices=['address', 'rename'], default='address')
parser.add_argument('--long', action='store_true')
parser.add_argument('--theme', type=int, choices=[1, 2], default=2)
args = parser.parse_args()
get_style = live.bind(live.user, 'GetWindowLongPtrW', live.PTR, W.HWND, C.c_int)
set_style = live.bind(live.user, 'SetWindowLongPtrW', live.PTR, W.HWND, C.c_int, live.PTR)
set_prop = live.bind(live.user, 'SetPropW', W.BOOL, W.HWND, W.LPCWSTR, W.HANDLE)
get_prop = live.bind(live.user, 'GetPropW', W.HANDLE, W.HWND, W.LPCWSTR)
original_send = live.send
configured = set()


def send(hwnd, msg, wp, lp):
    if msg == 0xC and hwnd not in configured:
        configured.add(hwnd)
        if args.presentation == 'uploaded':
            style = get_style(hwnd, -20)
            set_style(hwnd, -20, style & ~0x80000)
            set_style(hwnd, -20, style | 0x80000)
        elif args.presentation == 'native':
            if not set_prop(hwnd, 'Pulse.NativeEditFallback', W.HANDLE(1)):
                raise C.WinError(C.get_last_error())
    value = original_send(hwnd, msg, wp, lp)
    if args.expect_composed and msg in [0xC, 0x100] and hwnd in configured and wp != 0x1B:
        if not get_prop(hwnd, 'Pulse.EditComposition'):
            raise RuntimeError('Expected persistent composition surface, but no active surface exists')
    return value


live.send = send
live.user.SetProcessDpiAwarenessContext(C.c_void_p(-4))
previous = live.foreground()
try:
    text_value = ('Long_Pulse_中文_1234567890_' * 10 + 'end.txt') if args.long else 'Pulse_中文_123.txt'
    caret = len(text_value) - 3 if args.long else 3
    result = live.run(args.executable.resolve(), args.label, args.mode, args.kind,
                      text_value=text_value, caret=caret, theme=args.theme)
    result['presentation'] = args.presentation
    result['composed_verified'] = args.expect_composed
    result['mode'] = args.mode
    result['long_text'] = args.long
    result['theme'] = args.theme
    counts = []
    for endpoint in ['left', 'right']:
        pixels = np.asarray(Image.open(live.ROOT / f'{args.label}-{args.kind}-{endpoint}.png')).astype(np.int16)
        background = np.median(pixels.reshape(-1, 3), axis=0)
        counts.append(int(np.count_nonzero(np.max(np.abs(pixels - background), axis=2) > 40)))
    result['endpoint_ink_pixels'] = counts
    result['text_visible'] = all(value > 100 for value in counts)
    result['accepted'] = result['text_visible'] and result['unexpected'] == 0
    result['limits'] = 'One editor sample using synchronous key messages; not physical input, IME or full visual acceptance.'
    (live.ROOT / f'{args.label}-probe-result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
finally:
    if previous:
        live.activate(previous)
