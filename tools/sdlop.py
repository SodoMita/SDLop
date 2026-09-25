#!/usr/bin/env python3
"""
sdlop.py -- header generator for SDLop.

SDLop reimplements a subset of the SDL3 API (windowing, input, timing) with the
*exact same* declarations as upstream SDL3.  This script performs that extraction
mechanically from the upstream public headers, so the ABI can never drift:

  * every item (function, typedef, struct, enum, macro) that is kept is copied
    byte-for-byte from the upstream header (minus comments and indentation);
  * every item that is dropped is recorded in tools/dropped.txt, which
    tools/check_api.py uses as the allow-list for its verification pass.

Usage:
    tools/sdlop.py            regenerate include/SDL3/*.h
    tools/sdlop.py --check    exit(1) if regeneration would change anything
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "tools", "reference", "SDL3")
OUT = os.path.join(ROOT, "include", "SDL3")
SDL_VERSION = "3.2.10"

# ---------------------------------------------------------------------------
# What SDLop implements, per upstream header.
# 'name' is the item name as returned by name_of_decl(): a function name, a macro
# name, or "struct SDL_Foo" / "enum SDL_Foo" / "typedef ...".
# ---------------------------------------------------------------------------

FILES = {
    "SDL_assert.h": {
        "banner": "Assertions. The macro machinery is upstream's; SDLop implements\n"
                  " * SDL_ReportAssertion(), the handler plumbing and SDL_ResetAssertionReport().",
        "keep": [r"^enum SDL_AssertState", r"^SDL_AssertState$", r"^struct SDL_AssertData$",
                 r"^SDL_AssertData$", r"^SDL_AssertionHandler$", r"^SDL_AssertBreakpoint$",
                 r"^SDL_ReportAssertion$", r"^SDL_SetAssertionHandler$",
                 r"^SDL_GetDefaultAssertionHandler$", r"^SDL_GetAssertionHandler$",
                 r"^SDL_ResetAssertionReport$", r"^SDL_ASSERT_LEVEL", r"^SDL_assert",
                 r"^SDL_disabled_assert$", r"^SDL_enabled_assert$", r"^SDL_NULL_WHILE_LOOP"],
    },
    "SDL_error.h": {
        "banner": "Error reporting.",
        "keep": [r"^SDL_(SetError|GetError|ClearError|OutOfMemory|InvalidParamError|Unsupported)$"],
    },
    "SDL_log.h": {
        "banner": "Logging. Kept whole: it is small, dependency-free and the SDLop build has no\n * other way to report trouble.",
        "keep": [r"^SDL_LOG_", r"^SDL_Log", r"^SDL_SetLog", r"^SDL_GetLog",
                 r"^SDL_ResetLog", r"^enum SDL_Log", r"^SDL_LogOutputFunction$"],
    },
    "SDL_properties.h": {
        "banner": "Property bags (used by SDL_CreateWindowWithProperties and SDL_GetWindowProperties).",
        "keep": [r"^SDL_(CreateProperties|DestroyProperties|GetPropertyType|GetPropertyName|"
                 r"GetGlobalProperties|GetNumberProperty|GetStringProperty|GetProperty|"
                 r"SetNumberProperty|SetFloatProperty|SetBooleanProperty|SetStringProperty|"
                 r"SetProperty|HasProperty|ClearProperty|CopyProperties|EnumerateProperties|"
                 r"PropertiesID|PropertyType|PropertiesEnumerationCallback|INVALID_PROPERTY_ID|"
                 r"GetBooleanProperty|GetFloatProperty|GetPointerProperty|SetPointerProperty|"
                 r"SetPointerPropertyWithCleanup|LockProperties|UnlockProperties|"
                 r"CleanupPropertyCallback)$",
                 r"^enum SDL_PropertyType"],
    },
    "SDL_hints.h": {
        "banner": "Hints. Every upstream SDL_HINT_* name is kept so existing code compiles; only the\n"
                  " * ones listed in docs/HINTS.md actually change behaviour.",
        "keep": [r"^SDL_HINT_", r"^enum SDL_HintPriority", r"^SDL_HintPriority$",
                 r"^SDL_SetHint", r"^SDL_ResetHint", r"^SDL_GetHint",
                 r"^SDL_AddHintCallback$", r"^SDL_RemoveHintCallback$", r"^SDL_HintCallback$"],
    },
    "SDL_init.h": {
        "includes": ['SDL_events.h'],
        "banner": "Subsystem init. SDLop implements SDL_INIT_VIDEO (which implies SDL_INIT_EVENTS);\n"
                  " * the other subsystem flags are accepted, counted and allocate nothing.",
        "keep": [r"^SDL_INIT_", r"^SDL_Init$", r"^SDL_InitSubSystem$", r"^SDL_QuitSubSystem$",
                 r"^SDL_WasInit$", r"^SDL_Quit$", r"^enum SDL_InitFlags", r"^SDL_InitFlags$",
                 r"^SDL_SetAppMetadata", r"^SDL_GetAppMetadata", r"^SDL_PROP_APP_METADATA_",
                 r"^enum SDL_AppResult", r"^SDL_AppResult$", r"^SDL_AppInit_func$",
                 r"^SDL_AppIterate_func$", r"^SDL_AppEvent_func$", r"^SDL_AppQuit_func$",
                 r"^SDL_IsMainThread$", r"^SDL_MainThreadCallback$",
                 r"^SDL_RunOnMainThread$"],
        "extra_after": '''
/**
 * The only SDL_Init flags that make SDLop allocate anything.
 *
 * \\since This is a SDLop extension, not part of SDL3.
 */
#define SDL_SDLOP_IMPLEMENTED_SUBSYSTEMS (SDL_INIT_VIDEO | SDL_INIT_EVENTS)
''',
    },
    "SDL_events.h": {
        "includes": ['SDL_keycode.h', 'SDL_keyboard.h', 'SDL_mouse.h', 'SDL_video.h'],
        "extra": "#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32",
        "banner": "Events: window, keyboard, text input and mouse. The SDL_Event union keeps\n"
                  " * upstream's `Uint8 padding[128]` member, so size/ABI still match SDL3 even\n"
                  " * though the dropped event kinds are no longer members of the union.",
        # SDL_TouchFingerEvent is upstream's own struct (kept above); the two ID
        # typedefs it is built from live in SDL_touch.h upstream, and touch is not
        # part of SDLop's surface, so they are declared here instead of pulling in
        # that header. The union member has to exist for both whatever the event
        # kinds an application can actually receive.
        "extra_before": '''
typedef Uint64 SDL_TouchID;
typedef Uint64 SDL_FingerID;
''',
        "keep": [r"^SDL_(CommonEvent|DisplayEvent|WindowEvent|KeyboardDeviceEvent|KeyboardEvent|"
                 r"TextEditingEvent|TextEditingCandidatesEvent|TextInputEvent|MouseDeviceEvent|"
                 r"MouseMotionEvent|MouseButtonEvent|MouseWheelEvent)$",
                 r"^enum SDL_EventType", r"^SDL_EventType$",
                 r"^enum SDL_EventAction", r"^SDL_EventAction$",
                 r"^union SDL_Event$", r"^SDL_EventFilter$", r"^SDL_EventFilter$",
                 r"^SDL_PumpEvents$", r"^SDL_PeepEvents$", r"^SDL_HasEvent", r"^SDL_FlushEvent",
                 r"^SDL_PollEvent$", r"^SDL_WaitEvent", r"^SDL_PushEvent$",
                 r"^SDL_SetEventFilter$", r"^SDL_GetEventFilter$", r"^SDL_AddEventWatch$",
                 r"^SDL_RemoveEventWatch$", r"^SDL_FilterEvents$", r"^SDL_SetEventEnabled$",
                 r"^SDL_EventEnabled$", r"^SDL_RegisterEvents$", r"^SDL_GetEventDescription$", r"^SDL_COMPILE_TIME_ASSERT$",
                 r"^SDL_UserEvent$", r"^SDL_QuitEvent$", r"^struct SDL_UserEvent$",
                 r"^struct SDL_QuitEvent$", r"^SDL_TouchFingerEvent$"],
        "drop_members": {
            "union SDL_Event": ("SDL_AudioDeviceEvent", "SDL_CameraDeviceEvent", "SDL_ClipboardEvent",
                                "SDL_DropEvent", "SDL_Gamepad", "SDL_Joy", "SDL_Pen",
                                "SDL_RenderEvent", "SDL_SensorEvent"),
        },
    },
    "SDL_keycode.h": {
        "includes": ['SDL_scancode.h'],
        "banner": "Keycodes, scancodes and modifiers. The full upstream tables are kept: they are\n"
                  " * compile-time constants, they cost no code, and they are what makes ported code\n"
                  " * build without edits.",
        "keep": [r"^SDLK_", r"^SDL_Keycode$", r"^SDL_Keymod$", r"^enum SDL_Keymod", r"^SDL_KMOD_",
                 r"^SDL_SCANCODE_TO_KEYCODE$"],
    },
    "SDL_keyboard.h": {
        "includes": ['SDL_keycode.h', 'SDL_video.h'],
        "banner": "Keyboard state, key names and text input (compose/IME).",
        "keep": [r"^SDL_KeyboardID$", r"^SDL_GetKeyboardFocus$", r"^SDL_GetKeyboardState$",
                 r"^SDL_ResetKeyboard$", r"^SDL_GetModState$", r"^SDL_SetModState$",
                 r"^SDL_GetKeyFromScancode$", r"^SDL_GetScancodeFromKey$", r"^SDL_GetKeyName$",
                 r"^SDL_GetScancodeName$", r"^SDL_GetScancodeFromName$", r"^SDL_GetKeyFromName$",
                 r"^SDL_StartTextInput", r"^SDL_TextInputActive$", r"^SDL_StopTextInput$",
                 r"^SDL_ClearComposition$", r"^SDL_IsTextInputShown$", r"^SDL_SetTextInputArea$",
                 r"^SDL_GetTextInputArea$", r"^SDL_HasScreenKeyboardSupport$",
                 r"^SDL_ScreenKeyboardShown$", r"^SDL_PROP_TEXTINPUT_", r"^enum SDL_Capitalization",
                 r"^SDL_Capitalization$", r"^enum SDL_TextInputType",
                 r"^SDL_TextInputType$"],
    },
    "SDL_mouse.h": {
        "includes": ['SDL_video.h', 'SDL_surface.h'],
        "banner": "Mouse state, relative mode, warping, cursors.",
        "keep": [r"^SDL_MouseID$", r"^enum SDL_SystemCursor", r"^SDL_SystemCursor$",
                 r"^enum SDL_MouseWheelDirection", r"^SDL_MouseWheelDirection$",
                 r"^SDL_BUTTON", r"^SDL_Cursor$", r"^SDL_MouseButtonFlags$",
                 r"^SDL_GetMouseFocus$", r"^SDL_GetMouseState$", r"^SDL_GetGlobalMouseState$",
                 r"^SDL_GetRelativeMouseState$", r"^SDL_WarpMouseInWindow$",
                 r"^SDL_WarpMouseGlobal$", r"^SDL_SetWindowRelativeMouseMode$",
                 r"^SDL_GetWindowRelativeMouseMode$", r"^SDL_CaptureMouse$",
                 r"^SDL_CreateCursor$", r"^SDL_CreateColorCursor$", r"^SDL_CreateSystemCursor$",
                 r"^SDL_SetCursor$", r"^SDL_GetCursor$", r"^SDL_GetDefaultCursor$",
                 r"^SDL_DestroyCursor$", r"^SDL_ShowCursor$", r"^SDL_HideCursor$",
                 r"^SDL_CursorVisible$"],
    },
    "SDL_timer.h": {
        "banner": "Timing: monotonic clocks, delays, timer callbacks (the event loop is built on these).",
        "keep": [r"^SDL_TimerID$", r"^SDL_TimerCallback$", r"^SDL_NSTimerCallback$", r"^SDL_GetTicks", r"^SDL_GetPerformance",
                 r"^SDL_Delay", r"^SDL_AddTimer", r"^SDL_RemoveTimer$", r"^SDL_(MS|NS|US|SECONDS)_",
                 r"^SDL_NS_(TO_|PER_)"],
    },
    "SDL_version.h": {
        "banner": "SDL_GetVersion() reports the SDL3 version whose API this build tracks;\n"
                  " * SDL_GetSDLopVersion() reports the SDLop version.",
        "keep": [r"^SDL_VERSION$", r"^SDL_VERSIONNUM", r"^SDL_COMPILEDVERSION",
                 r"^SDL_VERSION_ATLEAST$", r"^SDL_MAJOR_VERSION$", r"^SDL_MINOR_VERSION$",
                 r"^SDL_MICRO_VERSION$", r"^SDL_Version$", r"^SDL_GetVersion$"],
        "extra_after": '''
/**
 * Marker macro: SDLop builds define this, upstream SDL3 does not.
 *
 * \\since This is a SDLop extension, not part of SDL3.
 */
#define SDL_SDLOP 1
#define SDL_SDLOP_MAJOR_VERSION 0
#define SDL_SDLOP_MINOR_VERSION 1
#define SDL_SDLOP_MICRO_VERSION 0

typedef struct SDL_SDLopVersion
{
    int major;
    int minor;
    int micro;
} SDL_SDLopVersion;

/**
 * Get the version of the SDLop library that is actually linked.
 *
 * \\param ver a pointer filled in with the SDLop version.
 *
 * \\threadsafety It is safe to call this function from any thread.
 *
 * \\since This is a SDLop extension, not part of SDL3.
 */
extern SDL_DECLSPEC void SDLCALL SDL_GetSDLopVersion(SDL_SDLopVersion *ver);
''',
    },
    "SDL_vulkan.h": {
        "includes": ['SDL_video.h'],
        "banner": "Vulkan surface creation, including upstream's VkInstance/VkSurfaceKHR typedefs.",
        "keep": [r"SDL_Vulkan", r"SDL_VULKAN", r"^VK_DEFINE", r"^VkInstance$",
                 r"^VkPhysicalDevice$", r"^VkSurfaceKHR$", r"^VkAllocationCallbacks$",
                 r"^NO_SDL_VULKAN_TYPEDEFS$", r"^VULKAN_H_$"],
    },
    "SDL_video.h": {
        "includes": ["SDL_rect.h", "SDL_pixels.h", "SDL_properties.h", "SDL_surface.h"],
        "banner": "Display and window management, plus OpenGL context creation (upstream declares\n"
                  " * GL in SDL_video.h; there is no SDL_gl.h in SDL3).",
        "keep": [r"^SDL_DisplayID$", r"^SDL_WindowID$", r"^enum SDL_SystemTheme",
                 r"^SDL_SystemTheme$", r"^struct SDL_DisplayModeData$", r"^struct SDL_DisplayMode$",
                 r"^enum SDL_DisplayOrientation", r"^SDL_DisplayOrientation$", r"^SDL_Window$",
                 r"^SDL_WindowFlags$", r"^SDL_WINDOW_", r"^SDL_WINDOWPOS_", r"^SDL_PROP_WINDOW_", r"^SDL_PROP_DISPLAY_",
                 r"^SDL_PROP_GLOBAL_VIDEO_", r"^SDL_DisplayModeData$",
                 r"^enum SDL_FlashOperation",
                 r"^SDL_FlashOperation$", r"^SDL_GLContext$", r"^enum SDL_GL",
                 r"^SDL_GLAttr$", r"^SDL_GLProfile$", r"^SDL_GLContextFlag$",
                 r"^SDL_GLContextReleaseFlag$", r"^SDL_GLContextResetNotification$",
                 r"^SDL_GL_", r"^enum SDL_HitTestResult", r"^SDL_HitTestResult$", r"^SDL_HitTest$",
                 r"^SDL_GetNumVideoDrivers$", r"^SDL_GetVideoDriver$",
                 r"^SDL_GetCurrentVideoDriver$", r"^SDL_GetSystemTheme$", r"^SDL_GetDisplays$",
                 r"^SDL_GetPrimaryDisplay$", r"^SDL_GetDisplayProperties$",
                 r"^SDL_GetDisplayName$", r"^SDL_GetDisplayBounds$",
                 r"^SDL_GetDisplayUsableBounds$", r"^SDL_GetNaturalDisplayOrientation$",
                 r"^SDL_GetCurrentDisplayOrientation$", r"^SDL_GetDisplayContentScale$",
                 r"^SDL_GetFullscreenDisplayModes$", r"^SDL_GetClosestFullscreenDisplayMode$",
                 r"^SDL_GetDisplayForPoint$", r"^SDL_GetDisplayForRect$",
                 r"^SDL_GetDisplayForWindow$", r"^SDL_GetDesktopDisplayMode$",
                 r"^SDL_GetCurrentDisplayMode$", r"^SDL_GetWindowDisplayScale$",
                 r"^SDL_GetWindowPixelDensity$", r"^SDL_SetWindowFullscreenMode$",
                 r"^SDL_GetWindowFullscreenMode$", r"^SDL_GetWindowPixelFormat$",
                 r"^SDL_GetWindows$", r"^SDL_CreateWindow$", r"^SDL_CreatePopupWindow$",
                 r"^SDL_CreateWindowWithProperties$", r"^SDL_GetWindowID$",
                 r"^SDL_GetWindowFromID$", r"^SDL_GetWindowParent$",
                 r"^SDL_GetWindowProperties$", r"^SDL_GetWindowFlags$", r"^SDL_SetWindowTitle$",
                 r"^SDL_GetWindowTitle$", r"^SDL_SetWindowIcon$", r"^SDL_SetWindowPosition$",
                 r"^SDL_GetWindowPosition$", r"^SDL_SetWindowSize$", r"^SDL_GetWindowSize$",
                 r"^SDL_GetWindowSafeArea$", r"^SDL_SetWindowAspectRatio$",
                 r"^SDL_GetWindowAspectRatio$", r"^SDL_GetWindowBordersSize$",
                 r"^SDL_GetWindowSizeInPixels$", r"^SDL_SetWindowMinimumSize$",
                 r"^SDL_GetWindowMinimumSize$", r"^SDL_SetWindowMaximumSize$",
                 r"^SDL_GetWindowMaximumSize$", r"^SDL_SetWindowBordered$",
                 r"^SDL_SetWindowResizable$", r"^SDL_SetWindowAlwaysOnTop$", r"^SDL_ShowWindow$",
                 r"^SDL_HideWindow$", r"^SDL_RaiseWindow$", r"^SDL_MaximizeWindow$",
                 r"^SDL_MinimizeWindow$", r"^SDL_RestoreWindow$", r"^SDL_SetWindowFullscreen$",
                 r"^SDL_SyncWindow$", r"^SDL_WindowHasSurface$", r"^SDL_GetWindowSurface$",
                 r"^SDL_UpdateWindowSurface$", r"^SDL_UpdateWindowSurfaceRects$",
                 r"^SDL_DestroyWindowSurface$", r"^SDL_SetWindowKeyboardGrab$",
                 r"^SDL_SetWindowMouseGrab$", r"^SDL_GetWindowKeyboardGrab$",
                 r"^SDL_GetWindowMouseGrab$", r"^SDL_GetGrabbedWindow$",
                 r"^SDL_SetWindowMouseRect$", r"^SDL_GetWindowMouseRect$",
                 r"^SDL_SetWindowOpacity$", r"^SDL_GetWindowOpacity$", r"^SDL_SetWindowParent$",
                 r"^SDL_SetWindowModal$", r"^SDL_SetWindowFocusable$", r"^SDL_SetWindowHitTest$",
                 r"^SDL_FlashWindow$", r"^SDL_DestroyWindow$"],
    },
    "SDL_scancode.h": {
        "banner": "Hardware scancodes: one enum, copied verbatim.",
        "keep": [r"^enum SDL_Scancode", r"^SDL_Scancode$"],
    },
    "SDL_rect.h": {
        "banner": "SDL_Point/SDL_Rect/FPoint/FRect and the SDL_FORCE_INLINE helpers that go with them.",
        "keep": [r"^struct SDL_Point$", r"^struct SDL_FPoint$", r"^struct SDL_Rect$",
                 r"^struct SDL_FRect$", r"^SDL_RectEmpty", r"^SDL_RectsEqual",
                 r"^SDL_PointInRect", r"^SDL_PointInRectFloat", r"^SDL_RectEmptyFloat$",
                 r"^SDL_RectsEqualEpsilon$", r"^SDL_RectsEqualFloat$", r"^SDL_HasRectIntersection",
                 r"^SDL_GetRectIntersection", r"^SDL_GetRectUnion", r"^SDL_GetRectEnclosingPoints",
                 r"^SDL_GetRectAndLineIntersection", r"^SDL_RectToFRect$"],
    },
    "SDL_misc.h": {
        "banner": "SDL_OpenURL().",
        "keep": [r"^SDL_OpenURL$"],
    },
    "SDL_pixels.h": {
        "banner": "Pixel formats. Everything is a compile-time constant, but only the formats the\n"
                  " * window-surface path can read/write are actually implemented (see docs/SURFACE.md).",
        "keep": [r"^enum SDL_PixelFormat", r"^SDL_PixelFormat$", r"^SDL_PixelFormatDetails$",
                 r"^SDL_Palette$", r"^enum SDL_ColorType", r"^SDL_ColorType$",
                 r"^enum SDL_PackedLayout", r"^SDL_PackedLayout$",
                 r"^enum SDL_PackedOrder", r"^SDL_PackedOrder$",
                 r"^enum SDL_ArrayOrder", r"^SDL_ArrayOrder$",
                 r"^enum SDL_BitmapOrder", r"^SDL_BitmapOrder$",
                 r"^enum SDL_Colorspace", r"^SDL_Colorspace$",
                 r"^SDL_Color$", r"^SDL_FColor$",
                 r"^SDL_DEFINE_PIXELFORMAT$", r"^SDL_DEFINE_COLORSPACE$", r"^SDL_DEFINE_PIXELFOURCC$",
                 r"^SDL_ISCOLORSPACE_",
                 r"^SDL_PIXELTYPE$", r"^SDL_PIXELORDER$",
                 r"^SDL_PIXELLAYOUT$", r"^SDL_BITSPERPIXEL$", r"^SDL_BYTESPERPIXEL$",
                 r"^SDL_ISPIXELFORMAT", r"^SDL_PIXELFLAG$", r"^SDL_ALPHA_",
                 r"^SDL_COLORSPACE", r"^SDL_GetPixelFormatName$", r"^SDL_GetMasksForPixelFormat$",
                 r"^SDL_GetPixelFormatForMasks$", r"^SDL_GetPixelFormatDetails$",
                 r"^SDL_CreatePixelFormat$", r"^SDL_DestroyPixelFormat$",
                 r"^SDL_GetRGB$", r"^SDL_GetRGBA$", r"^SDL_MapRGB$", r"^SDL_MapRGBA$",
                 r"^SDL_PIXELFORMAT_"],
    },
    "SDL_surface.h": {
        "includes": ['SDL_pixels.h', 'SDL_rect.h', 'SDL_properties.h'],
        "banner": "Minimal surfaces: exactly what SDL_GetWindowSurface()/SDL_UpdateWindowSurface()\n"
                  " * need, plus SDL_FillSurfaceRect(). There is no blitter, no scaling and no\n"
                  " * SDL_Surface-to-SDL_Surface conversion.",
        "keep": [r"^SDL_Surface$", r"^SDL_CreateSurface$", r"^SDL_CreateSurfaceFrom$",
                 r"^SDL_DestroySurface$", r"^SDL_GetSurfaceProperties$", r"^SDL_SetSurfaceColorspace$",
                 r"^SDL_GetSurfaceColorspace$", r"^SDL_LockSurface$", r"^SDL_UnlockSurface$",
                 r"^SDL_MUSTLOCK$", r"^SDL_FillSurfaceRect$", r"^SDL_FillSurfaceRects$",
                 r"^SDL_MapSurfaceRGB$", r"^SDL_MapSurfaceRGBA$",
                 r"^SDL_GetSurfaceWidth$", r"^SDL_GetSurfaceHeight$",
                 r"^enum SDL_SurfaceFlags", r"^SDL_SurfaceFlags$", r"^SDL_FLIP_", r"^SDL_SURFACE_", r"^SDL_PROP_SURFACE"],
    },
}

# Preprocessor directives that are always carried over (they may wrap kept items).
KEEP_PREPROC = {"if", "ifdef", "ifndef", "else", "elif", "endif"}
# Directives that are never carried over.
DROP_PREPROC = {"include", "error", "pragma", "warning", "line"}


# ---------------------------------------------------------------------------
# Pass 1: remove comments, include guard and the extern "C" wrapper
# ---------------------------------------------------------------------------

GUARD_RX = re.compile(r"^\s*#\s*(ifndef|define)\s+(SDL_[A-Za-z0-9_]*_h_)\s*$")


def strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(text[i])
                elif text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def strip_glue(text):
    lines = text.split("\n")
    guard = None
    m = re.search(r"^\s*#\s*ifndef\s+(SDL_[A-Za-z0-9_]*_h_)\s*$", text, re.M)
    if m:
        guard = m.group(1)
    out, depth, i, n = [], 0, 0, len(lines)
    while i < n:
        raw = lines[i]
        st = raw.strip()
        m = re.match(r"^\s*#\s*(\w+)\s*(.*)$", raw)
        kw = m.group(1) if m else None
        if guard:
            if re.match(r"^\s*#\s*ifndef\s+" + re.escape(guard) + r"\s*$", raw) or \
               re.match(r"^\s*#\s*define\s+" + re.escape(guard) + r"\s*$", raw):
                i += 1
                continue
            if kw == "endif" and depth == 0:
                i += 1
                continue
        if kw in ("ifdef", "ifndef") and st.split()[1:2] == ["__cplusplus"]:
            i += 1
            while i < n and lines[i].strip() in ('extern "C" {', "}"):
                i += 1
            if i < n and lines[i].strip() == "#endif":
                i += 1
            continue
        if kw in ("if", "ifdef", "ifndef"):
            depth += 1
        elif kw == "endif" and depth:
            depth -= 1
        out.append(raw)
        i += 1
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Pass 2: split into items
# ---------------------------------------------------------------------------

class Item:
    __slots__ = ("kind", "name", "text")

    def __init__(self, kind, name, text):
        self.kind = kind      # 'define' | 'undef' | 'preproc' | 'decl'
        self.name = name
        self.text = text

    def __repr__(self):
        return "Item(%s,%s)" % (self.kind, self.name)


IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def tidy(text):
    lines = [ln.rstrip() for ln in text.strip().split("\n")]
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    if not lines:
        return ""
    indents = [len(ln) - len(ln.lstrip()) for ln in lines if ln.strip()]
    pad = min(indents) if indents else 0
    res = []
    for ln in lines:
        if not ln.strip():
            res.append("")
            continue
        body = ln[pad:] if ln[:pad].strip() == "" else ln.lstrip()
        lead = len(body) - len(body.lstrip())
        res.append(" " * lead + re.sub(r"[ \t]{2,}", " ", body.strip()))
    return "\n".join(res)


def name_of_decl(text):
    t = " ".join(text.split())
    m = re.search(r"typedef\s+(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", t)
    if m:
        return "%s %s" % (m.group(1), m.group(2))
    m = re.search(r"(?:^|\s)(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", t)
    if m:
        return "%s %s" % (m.group(1), m.group(2))
    if t.startswith("typedef"):
        # function-pointer typedef, e.g. typedef bool (SDLCALL *SDL_HitTest)(...)
        m = re.search(r"\(\s*(?:SDLCALL\s+|[A-Za-z_][A-Za-z0-9_]*\s+)?\*\s*"
                      r"([A-Za-z_][A-Za-z0-9_]*)\s*\)", t)
        if m:
            return m.group(1)
        mm = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", t.rstrip(";").strip())
        return mm.group(1) if mm else t[:60]
    # a function or a variable: the first identifier followed by '(' is the name
    # (this must NOT look for '(... *name)' here: a plain pointer parameter would
    # be mistaken for a function pointer and the declaration would be dropped).
    mm = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", t)
    if mm:
        return mm.group(1)
    mm = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*;", t)
    return mm.group(1) if mm else t[:60]


def parse_items(text):
    """Split preprocessed header text into items.

    A preprocessor directive at the start of a line becomes its own item, unless a
    declaration is currently open (e.g. `#if` inside an enum body), in which case
    the directive is kept as part of that declaration so the emitted declaration
    stays identical to upstream.
    """
    items, buf = [], []
    i, n = 0, len(text)
    depth = 0

    def complete():
        s = "".join(buf).rstrip()
        return s.endswith(";") or s.endswith("}")

    def flush(kind="decl"):
        raw = tidy("".join(buf))
        buf.clear()
        if not raw:
            return
        if kind == "preproc":
            m = re.match(r"#\s*(\w+)\s*(.*)", raw, re.S)
            if not m:
                return
            d, rest = m.group(1), m.group(2).strip()
            nm = IDENT.match(rest)
            nm = nm.group(0) if nm else rest[:30]
            if d == "define":
                items.append(Item("define", nm, raw))
            elif d == "undef":
                items.append(Item("undef", nm, raw))
            else:
                items.append(Item("preproc", d, raw))
        else:
            items.append(Item("decl", name_of_decl(raw), raw))

    while i < n:
        c = text[i]
        if c == "#" and (i == 0 or text[i - 1] == "\n") and \
                (not "".join(buf).strip() or complete()):
            if "".join(buf).strip():
                flush("decl")
            j = i
            while j < n:
                eol = text.find("\n", j)
                eol = n if eol < 0 else eol
                if text[j:eol].rstrip().endswith("\\"):
                    j = eol + 1
                    continue
                j = eol
                break
            buf.append(text[i:j])
            flush("preproc")
            i = j
            continue
        buf.append(c)
        if c == "(" or c == "{" or c == "[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        elif c == ";" and depth == 0:
            flush("decl")
        i += 1
    if "".join(buf).strip():
        flush("decl")
    return items


# ---------------------------------------------------------------------------
# Pass 3: filter + emit
# ---------------------------------------------------------------------------

def name_candidates(item):
    names = [item.name]
    parts = item.name.split()
    if len(parts) > 1:
        names.append(parts[-1])          # "struct SDL_Foo" -> "SDL_Foo"
    return names


def is_type_item(item):
    """True for declarations that only *define* a type (never a function we would
    have to link). Used by the dependency closure below."""
    t = item.text.lstrip()
    return t.startswith("typedef") or t.startswith("struct ") or \
        t.startswith("union ") or t.startswith("enum ")


def close_over(spec, items):
    """Keep every type a kept declaration refers to.

    Without this, trimming a header by hand silently drops things like
    SDL_SurfaceFlags or a callback typedef that a kept function's signature
    mentions. The closure only ever pulls in type definitions, never function
    declarations, so it cannot smuggle in unimplemented code.
    """
    kept = [it for it in items if keep_item(spec, it)]
    kept_ids = set(id(it) for it in kept)
    while True:
        text = "\n".join(it.text for it in kept)
        referenced = set(re.findall(r"\b(SDL_[A-Za-z0-9_]+)\b", text))
        added = False
        for it in items:
            if id(it) in kept_ids or not is_type_item(it):
                continue
            if any(nm in referenced for nm in name_candidates(it)):
                kept.append(it)
                kept_ids.add(id(it))
                added = True
        if not added:
            break
    order = {id(it): i for i, it in enumerate(items)}
    kept.sort(key=lambda it: order[id(it)])
    return kept


def keep_item(spec, item):
    if item.kind == "preproc":
        return item.name in KEEP_PREPROC
    if item.kind == "decl" and item.name in spec.get("force_drop", ()):
        return False
    names = [item.name]
    parts = item.name.split()
    if len(parts) > 1:
        names.append(parts[-1])          # "struct SDL_Foo" -> "SDL_Foo"
    for rx in spec["keep"]:
        for nm in names:
            if re.search(rx, nm):
                return True
    return False


def drop_members(text, prefixes):
    m = re.search(r"\{(.*)\}", text, re.S)
    if not m:
        return text
    body = m.group(1)
    keep = []
    for chunk in body.split("\n"):
        st = chunk.strip()
        if st and any(st.startswith(p) for p in prefixes):
            continue
        keep.append(chunk)
    return text[:m.start(1)] + "\n".join(keep) + text[m.end(1):]


def normalize(text):
    """Strip redundant spaces but keep upstream's line structure."""
    return "\n".join(re.sub(r"[ \t]{2,}", " ", ln) for ln in text.split("\n"))


HEADER = '''/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header {src} ({ver}), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: {banner}
*/

#ifndef {guard}
#define {guard}

#include <SDL3/SDL_begin_code.h>
{includes}
#ifdef __cplusplus
extern "C" {{
#endif
'''


def emit(fname, spec, items, dropped):
    guard = "SDL_%s" % re.sub(r"[^A-Za-z0-9]", "_", fname).upper()
    m = re.search(r"^\s*#\s*ifndef\s+(SDL_[A-Za-z0-9_]*_h_)\s*$", spec.get("_raw", ""), re.M)
    if m:
        guard = m.group(1)
    incs = "".join("#include <SDL3/%s>\n" % i for i in spec.get("includes", []))
    out = [HEADER.format(src=fname, ver=SDL_VERSION, banner=spec.get("banner", ""),
                         guard=guard, includes=incs)]
    if spec.get("extra_before"):
        out.append(spec["extra_before"].strip("\n"))
        out.append("")
    kept = 0
    kept_items = close_over(spec, items)
    kept_ids = set(id(it) for it in kept_items)
    for it in items:
        if id(it) in kept_ids:
            out.append(normalize(it.text))
            out.append("")
            kept += 1
        elif it.kind in ("decl", "define", "undef"):
            dropped.append("%s\t%s\t%s" % (fname, it.kind, it.name))
    if spec.get("extra"):
        out.append(spec["extra"].strip("\n"))
        out.append("")
    if spec.get("extra_after"):
        out.append(spec["extra_after"].strip("\n"))
        out.append("")
    # dropped union members leave runs of empty lines behind; squash them
    squashed = []
    for line in "\n".join(out).split("\n"):
        if not line.strip() and squashed and not squashed[-1].strip():
            continue
        squashed.append(line)
    out = squashed
    out.append('#include <SDL3/SDL_close_code.h>')
    out.append("")
    out.append("#ifdef __cplusplus")
    out.append("}")
    out.append("#endif")
    out.append("")
    out.append("#endif /* %s */" % guard)
    return "\n".join(out) + "\n", kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    dropped, changed = [], []
    total_kept = 0
    print("%-20s %5s %5s %7s" % ("header", "items", "kept", "bytes"))
    for fname, spec in FILES.items():
        with open(os.path.join(REF, fname), encoding="utf-8", errors="replace") as fh:
            spec["_raw"] = fh.read()          # raw text: where emit() looks up the guard
        text = strip_glue(strip_comments(spec["_raw"]))
        items = parse_items(text)
        for it in items:
            if it.kind == "decl" and it.name in spec.get("drop_members", {}):
                it.text = drop_members(it.text, spec["drop_members"][it.name])
        out, kept = emit(fname, spec, items, dropped)
        total_kept += kept
        dst = os.path.join(OUT, fname)
        old = open(dst).read() if os.path.exists(dst) else None
        if old != out:
            changed.append(fname)
            if not args.check:
                with open(dst, "w") as fh:
                    fh.write(out)
        print("%-20s %5d %5d %7d" % (fname, len(items), kept, len(out)))
    with open(os.path.join(ROOT, "tools", "dropped.txt"), "w") as fh:
        fh.write("# Everything SDL3 %s declares that SDLop deliberately does not implement.\n"
                 "# Generated by tools/sdlop.py; used as the allow-list of tools/check_api.py.\n"
                 "# Format: <header>\\t<kind>\\t<name>\n\n" % SDL_VERSION)
        for line in sorted(set(dropped)):
            fh.write(line + "\n")
    print("\ntotal kept: %d items; dropped: %d (tools/dropped.txt)"
          % (total_kept, len(set(dropped))))
    if args.check and changed:
        print("\nOUT OF DATE (run tools/sdlop.py): %s" % ", ".join(changed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
