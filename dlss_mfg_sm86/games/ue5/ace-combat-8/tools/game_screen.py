import ctypes
import pathlib
import sys
import time
from PIL import ImageGrab
from ctypes import wintypes

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()
hwnd = user32.FindWindowW(None, 'ACE COMBAT 8 : WINGS OF THEVE')
if not hwnd:
    raise SystemExit('Game window not found')
rect = wintypes.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rect))
if len(sys.argv) > 1:
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    if user32.GetForegroundWindow() != hwnd:
        raise SystemExit('Refusing input: game is not foreground')
    if sys.argv[1] == 'key':
        keys = {'enter': 0x0d, 'escape': 0x1b, 'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27}
        key = keys[sys.argv[2]]
        user32.keybd_event(key, 0, 0, 0)
        time.sleep(0.1)
        user32.keybd_event(key, 0, 2, 0)
    elif sys.argv[1] == 'click':
        x, y = int(sys.argv[2]), int(sys.argv[3])
        if not (0 <= x < rect.right - rect.left and 0 <= y < rect.bottom - rect.top):
            raise SystemExit('Click outside game')
        user32.SetCursorPos(rect.left + x, rect.top + y)
        user32.mouse_event(2, 0, 0, 0, 0)
        time.sleep(0.1)
        user32.mouse_event(4, 0, 0, 0, 0)
    else:
        raise SystemExit('Unknown action')
    time.sleep(3)
print('Window:', hwnd, 'bounds:', rect.left, rect.top, rect.right, rect.bottom)
path = pathlib.Path(__file__).parent.parent / 'research' / 'ac8-screen.png'
ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom), all_screens=True).save(path)
print(path)
