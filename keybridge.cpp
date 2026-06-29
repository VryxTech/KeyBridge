// ═══════════════════════════════════════════════════════════════════════════
//  KeyBridge
//  Developed by VryxTech
// ═══════════════════════════════════════════════════════════════════════════

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <gdiplus.h>
#include "resource.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Gdiplus;

// ═══════════════════════════════════════════════════
//  COLORS
// ═══════════════════════════════════════════════════
#define CLR_BG       RGB(15,  16,  23)
#define CLR_SURFACE  RGB(22,  23,  32)
#define CLR_SURFACE2 RGB(28,  29,  40)
#define CLR_BORDER   RGB(40,  41,  56)
#define CLR_ACCENT   RGB(129, 115, 250)
#define CLR_ACCENT2  RGB(64,  55,  136)
#define CLR_GREEN    RGB(74,  222, 128)
#define CLR_RED      RGB(248, 113, 113)
#define CLR_TEXT     RGB(230, 232, 245)
#define CLR_DIM      RGB(120, 123, 148)
#define CLR_MUTED    RGB(58,  60,  82)

// ═══════════════════════════════════════════════════
//  CONSTANTS
// ═══════════════════════════════════════════════════
#define WM_TRAY          (WM_APP + 1)
#define ID_SHOW          4001
#define ID_EXIT          4002
#define ID_TOGGLEAPP     4003
#define HK_TOGGLE        2003
#define ID_ALTGR_TIMER   3010

static constexpr UINT ALTGR_DELAY_MS = 8;

static constexpr int CARD_H      = 72;
static constexpr int CARD_GAP    = 10;
static constexpr int LIST_TOP    = 140;
static constexpr int HEADER_H    = 112;
static constexpr int FOOTER_H    = 24;

static constexpr int PICK_W      = 560;
static constexpr int PICK_H      = 380;

static constexpr int MIN_W       = 640;
static constexpr int MIN_H       = 480;
static constexpr int CFG_VERSION = 7;

static constexpr const wchar_t* kMainWindowClass   = L"KeyBridgeMainWindow";
static constexpr const wchar_t* kPickerWindowClass = L"KeyBridgePicker";

// ═══════════════════════════════════════════════════
//  DATA
// ═══════════════════════════════════════════════════
struct KeyMap {
    int   id;
    DWORD src;
    DWORD tgt;
    bool  active;
};

struct HitResult {
    int card;
    int btn;
};

struct CardLayout {
    int cardX, cardY, cardW;
    int chipW, chipH;
    int srcX, srcY;
    int tgtX, tgtY;
    int arrowX;
    int togX, togY;
    int delX, delY;

    static CardLayout Compute(int windowW, int cardIdx, int scrollY) {
        CardLayout cl{};
        cl.cardW  = std::max(260, windowW - 40);
        cl.cardX  = 20;
        cl.cardY  = LIST_TOP - scrollY + cardIdx * (CARD_H + CARD_GAP);

        cl.chipW  = std::max(120, std::min(190, (cl.cardW - 260) / 2));
        cl.chipH  = 32;

        cl.srcX   = cl.cardX + 18;
        cl.srcY   = cl.cardY + 28;

        cl.tgtX   = cl.cardX + cl.cardW - 18 - cl.chipW - 94;
        cl.tgtY   = cl.srcY;

        cl.arrowX = (cl.srcX + cl.chipW + cl.tgtX) / 2 - 16;

        cl.togX   = cl.cardX + cl.cardW - 84;
        cl.togY   = cl.cardY + 24;

        cl.delX   = cl.cardX + cl.cardW - 36;
        cl.delY   = cl.cardY + 22;

        return cl;
    }
};

struct AltGrState {
    bool pending   = false;
    bool committed = false;
    bool altgr     = false;
};

// ═══════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════
static HHOOK              g_hook             = nullptr;
static HWND               g_hwnd             = nullptr;
static HINSTANCE          g_inst             = nullptr;
static bool               g_active           = true;
static int                g_nextId           = 1;
static int                g_hover            = -1;
static int                g_hBtn             = -1;
static int                g_scrollY          = 0;
static int                g_scrollMax        = 0;
static int                g_wheelRemainder   = 0;

static NOTIFYICONDATAW    g_nid              = {};
static bool               g_trayOk           = false;
static bool               g_pickActive       = false;
static bool               g_pickDone         = false;
static bool               g_trayTipShown     = false;
static bool               g_pickClassOk      = false;
static bool               g_hotkeyRegistered = false;
static bool               g_gdiPlusOk        = false;
static UINT               g_dpi              = 96;

static HCURSOR            g_curArrow         = nullptr;
static HCURSOR            g_curHand          = nullptr;
static AltGrState         g_altGr            {};
static ULONG_PTR          g_gdiplusToken     = 0;

static std::wstring       g_pickTitle;
static std::wstring       g_pickHint;

static std::vector<KeyMap>                g_maps;
static std::unordered_map<DWORD, DWORD>   g_map;
static std::unordered_set<DWORD>          g_injectedDown;

static UINT g_hkMod = MOD_CONTROL;
static UINT g_hkVk  = VK_F12;

// Scrollbar dragging
static bool g_scrollDragging   = false;
static int  g_scrollDragOffset = 0;

// ── Settings globals ────────────────────────────────
static bool g_startWithWindows = false;
static bool g_startMinimized   = false;
static bool g_closeToTray      = false;
static HWND g_hwndSettings     = nullptr;
static int  g_settingsHoverBtn = 0;   // 0: none, 1: OK, 2: Cancel

// ─── Logo rendering system ─────────────────────────────
namespace Logo {
    static std::unique_ptr<Gdiplus::Image> s_logo;

    bool Initialize() {
        HRSRC hRes = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
        if (!hRes) {
            OutputDebugStringW(L"[KeyBridge] Failed to find embedded logo resource.\n");
            return false;
        }

        DWORD size = SizeofResource(g_inst, hRes);
        HGLOBAL hGlobal = LoadResource(g_inst, hRes);
        if (!hGlobal) {
            OutputDebugStringW(L"[KeyBridge] Failed to load embedded logo resource.\n");
            return false;
        }

        void* data = LockResource(hGlobal);
        if (!data) {
            OutputDebugStringW(L"[KeyBridge] Failed to lock embedded logo resource.\n");
            return false;
        }

        HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!hBuffer) {
            OutputDebugStringW(L"[KeyBridge] Failed to allocate memory for logo.\n");
            return false;
        }

        // FIX: GlobalLock can return NULL under memory pressure; check before use.
        void* pBuffer = GlobalLock(hBuffer);
        if (!pBuffer) {
            GlobalFree(hBuffer);
            OutputDebugStringW(L"[KeyBridge] GlobalLock failed for logo buffer.\n");
            return false;
        }
        memcpy(pBuffer, data, size);
        GlobalUnlock(hBuffer);

        IStream* pStream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(hBuffer, TRUE, &pStream))) {
            GlobalFree(hBuffer);
            OutputDebugStringW(L"[KeyBridge] Failed to create stream for logo.\n");
            return false;
        }

        s_logo.reset(Gdiplus::Image::FromStream(pStream));
        pStream->Release();

        if (!s_logo || s_logo->GetLastStatus() != Gdiplus::Ok) {
            OutputDebugStringW(L"[KeyBridge] Failed to load embedded logo resource.\n");
            s_logo.reset();
            return false;
        }

        return true;
    }

    void Draw(HDC hdc, int x, int y, int w, int h) {
        if (!s_logo) return;

        Gdiplus::Graphics g(hdc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        const float logoW = (float)s_logo->GetWidth();
        const float logoH = (float)s_logo->GetHeight();
        const float scale = std::min((float)w / logoW, (float)h / logoH);
        const float drawW = logoW * scale;
        const float drawH = logoH * scale;
        const int drawX = x + (int)((w - drawW) / 2.0f);
        const int drawY = y + (int)((h - drawH) / 2.0f);

        g.DrawImage(s_logo.get(), drawX, drawY, (int)drawW, (int)drawH);
    }

    void Shutdown() {
        s_logo.reset();
    }
}

// Forward declarations
LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK PickProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp);

void RebuildMap();
void SyncTrayTip();
void SetAppActive(bool on);
bool SaveConfig();
void LoadConfig();
bool TryRegisterToggleHotkey(UINT mod, UINT vk);
void UnregisterToggleHotkey();
DWORD PickKey(HWND parent, const wchar_t* title, bool hotkeyMode = false);
void UpdateScroll(HWND hw);
void SetAppDpi(HWND hw);

void FillRoundRect(HDC dc, int x, int y, int w, int h, int r, COLORREF c);
void DrawRoundBorder(HDC dc, int x, int y, int w, int h, int r, COLORREF c, int thick = 1);
void DrawTextModern(HDC dc, const std::wstring& text,
                    int x, int y, int w, int h,
                    COLORREF color,
                    int fontSize = 14, bool bold = false,
                    UINT fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE);

void RestoreMainWindow(HWND hw);
void ShowTrayMenu(HWND hw);
void HandleTrayEvent(HWND hw, LPARAM lp);
void OnPaint(HWND hw);
HitResult HitTest(HWND hw, int mx, int my);

bool EnsurePickClassRegistered();
void FinalizePick(DWORD vk);

bool SendRawKey(DWORD vk, bool isDown);
bool InjectMappedKey(DWORD srcVk, bool isDown);
bool ProcessAltGrAwareEvent(const KBDLLHOOKSTRUCT* k, bool isDown, bool& passThrough);

static Color ToGdiPlus(COLORREF c, BYTE a = 255) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// ═══════════════════════════════════════════════════
//  FONT CACHE  (covers sizes 9–20, no per-draw alloc)
// ═══════════════════════════════════════════════════
struct FontCache {
    static constexpr int kMinSz = 9;
    static constexpr int kMaxSz = 20;
    static constexpr int kCount = kMaxSz - kMinSz + 1;

    HFONT normal[kCount] = {};
    HFONT bold_[kCount]  = {};

    void Destroy() {
        for (auto& f : normal) { if (f) { DeleteObject(f); f = nullptr; } }
        for (auto& f : bold_)  { if (f) { DeleteObject(f); f = nullptr; } }
    }

    void Init(UINT dpi = 96) {
        Destroy();
        for (int sz = kMinSz; sz <= kMaxSz; ++sz) {
            const int h = -MulDiv(sz, (int)dpi, 96);
            auto mk = [&](bool b) -> HFONT {
                return CreateFontW(
                    h, 0, 0, 0,
                    b ? FW_BOLD : FW_NORMAL,
                    FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY,
                    DEFAULT_PITCH,
                    L"Segoe UI");
            };
            normal[sz - kMinSz] = mk(false);
            bold_[sz - kMinSz]  = mk(true);
            // FIX: Diagnostic for font creation failure; guards in DrawTextModern already
            // handle NULL safely, but logging here aids debugging.
            if (!normal[sz - kMinSz] || !bold_[sz - kMinSz]) {
                OutputDebugStringW(L"[KeyBridge] FontCache: CreateFontW returned NULL.\n");
            }
        }
    }

    HFONT Get(int sz, bool b) const {
        sz = std::clamp(sz, kMinSz, kMaxSz);
        return b ? bold_[sz - kMinSz] : normal[sz - kMinSz];
    }
} g_fc;

// ═══════════════════════════════════════════════════
//  KEY NAMES
// ═══════════════════════════════════════════════════
static const std::unordered_map<DWORD, std::wstring> g_keyNames = {
    {VK_LEFT,   L"Left Arrow"},    {VK_RIGHT,  L"Right Arrow"},
    {VK_UP,     L"Up Arrow"},      {VK_DOWN,   L"Down Arrow"},
    {VK_RMENU,  L"Right Alt"},     {VK_LMENU,  L"Left Alt"},
    {VK_RCONTROL, L"Right Ctrl"},  {VK_LCONTROL, L"Left Ctrl"},
    {VK_RSHIFT, L"Right Shift"},   {VK_LSHIFT, L"Left Shift"},
    {VK_APPS,   L"Menu"},
    {VK_DELETE, L"Delete"},        {VK_INSERT, L"Insert"},
    {VK_HOME,   L"Home"},          {VK_END,    L"End"},
    {VK_PRIOR,  L"Page Up"},       {VK_NEXT,   L"Page Down"},
    {VK_BACK,   L"Backspace"},     {VK_RETURN, L"Enter"},
    {VK_TAB,    L"Tab"},           {VK_ESCAPE, L"Escape"},
    {VK_SPACE,  L"Space"},         {VK_CAPITAL, L"Caps Lock"},
    {VK_NUMLOCK, L"Num Lock"},     {VK_SCROLL, L"Scroll Lock"},
    {VK_SNAPSHOT, L"Print Screen"}, {VK_PAUSE, L"Pause"},
    {VK_F1, L"F1"}, {VK_F2, L"F2"}, {VK_F3, L"F3"}, {VK_F4, L"F4"},
    {VK_F5, L"F5"}, {VK_F6, L"F6"}, {VK_F7, L"F7"}, {VK_F8, L"F8"},
    {VK_F9, L"F9"}, {VK_F10, L"F10"}, {VK_F11, L"F11"}, {VK_F12, L"F12"},
    {VK_NUMPAD0, L"Num 0"}, {VK_NUMPAD1, L"Num 1"}, {VK_NUMPAD2, L"Num 2"},
    {VK_NUMPAD3, L"Num 3"}, {VK_NUMPAD4, L"Num 4"}, {VK_NUMPAD5, L"Num 5"},
    {VK_NUMPAD6, L"Num 6"}, {VK_NUMPAD7, L"Num 7"}, {VK_NUMPAD8, L"Num 8"},
    {VK_NUMPAD9, L"Num 9"}, {VK_MULTIPLY, L"Num *"}, {VK_ADD, L"Num +"},
    {VK_SUBTRACT, L"Num -"}, {VK_DIVIDE, L"Num /"}, {VK_DECIMAL, L"Num ."},
    {0xBE, L"Period (.)"}, {0xBC, L"Comma (,)"},
    {0xBF, L"Slash (/)"}, {0xBD, L"Minus (-)"},
    {0xBB, L"Equals (=)"}, {0xC0, L"Tilde (~)"},
    {0xDB, L"Left Bracket ["}, {0xDD, L"Right Bracket ]"},
    {0xDC, L"Backslash"},  {0xDE, L"Quote"},     {0xBA, L"Semicolon"},
    {VK_SHIFT, L"Shift"}, {VK_CONTROL, L"Ctrl"}, {VK_MENU, L"Alt"},
    {VK_LWIN, L"Left Win"}, {VK_RWIN, L"Right Win"},
};

std::wstring GetKeyName(DWORD vk) {
    auto it = g_keyNames.find(vk);
    if (it != g_keyNames.end()) return it->second;
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    wchar_t buf[32];
    StringCchPrintfW(buf, 32, L"VK_%02X", (unsigned)vk);
    return buf;
}

std::wstring GetHotkeyString(UINT mod, UINT vk) {
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl+";
    if (mod & MOD_SHIFT)   s += L"Shift+";
    if (mod & MOD_ALT)     s += L"Alt+";
    if (mod & MOD_WIN)     s += L"Win+";
    s += GetKeyName(vk);
    return s;
}

std::wstring GetHotkeyString() {
    return GetHotkeyString(g_hkMod, g_hkVk);
}

// ═══════════════════════════════════════════════════
//  DPI / DWM / GDI+
// ═══════════════════════════════════════════════════
static void SetDpiAwareness() {
    using PFN_SetProcessDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
    using PFN_SetProcessDpiAwareness        = HRESULT(WINAPI*)(int);

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        auto fn = (PFN_SetProcessDpiAwarenessContext)
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return;
    }

    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        auto fn2 = (PFN_SetProcessDpiAwareness)
            GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (fn2) fn2(2);
        FreeLibrary(hShcore);
        return;
    }

    SetProcessDPIAware();
}

static void ApplyWindowEffects(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, (DWMWINDOWATTRIBUTE)20, &dark, sizeof(dark));
    int corner = 2;
    DwmSetWindowAttribute(hwnd, (DWMWINDOWATTRIBUTE)33, &corner, sizeof(corner));
}

void SetAppDpi(HWND hw) {
    if (!hw) return;
    UINT dpi = 96;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
        auto p = (GetDpiForWindow_t)GetProcAddress(u32, "GetDpiForWindow");
        if (p) dpi = p(hw);
    }
    g_dpi = dpi;
    g_fc.Init(g_dpi);
}

static bool StartGdiPlus() {
    GdiplusStartupInput input{};
    input.GdiplusVersion = 1;
    return GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Ok;
}

static void StopGdiPlus() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

// ═══════════════════════════════════════════════════
//  CONFIG PATH
// ═══════════════════════════════════════════════════
std::wstring GetCfgPath() {
    static std::wstring path;
    if (!path.empty()) return path;

    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA,
                                   nullptr, SHGFP_TYPE_CURRENT, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\KeyBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        path = dir + L"\\keybridge.cfg";
        return path;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring tmp(exePath);
    auto pos = tmp.rfind(L'\\');
    if (pos != std::wstring::npos) tmp = tmp.substr(0, pos);
    path = tmp + L"\\keybridge.cfg";
    return path;
}

// ═══════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════
static UINT NormalizeHotkeyMod(UINT mod) {
    UINT out = 0;
    if (mod & MOD_CONTROL) out |= MOD_CONTROL;
    if (mod & MOD_SHIFT)   out |= MOD_SHIFT;
    if (mod & MOD_ALT)     out |= MOD_ALT;
    if (mod & MOD_WIN)     out |= MOD_WIN;
    return out;
}

static bool IsModifierVk(DWORD vk) {
    switch (vk) {
        case VK_SHIFT:    case VK_LSHIFT:   case VK_RSHIFT:
        case VK_CONTROL:  case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU:     case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:     case VK_RWIN:
            return true;
        default:
            return false;
    }
}

static bool CanUseAsMappingTarget(DWORD vk) {
    return vk != 0;
}

static bool IsExtendedKey(DWORD vk) {
    switch (vk) {
        case VK_LEFT:     case VK_RIGHT:    case VK_UP:      case VK_DOWN:
        case VK_INSERT:   case VK_DELETE:   case VK_HOME:    case VK_END:
        case VK_PRIOR:    case VK_NEXT:     case VK_RCONTROL:case VK_RMENU:
        case VK_NUMLOCK:  case VK_SNAPSHOT: case VK_APPS:
        case VK_LWIN:     case VK_RWIN:     case VK_DIVIDE:
            return true;
        default:
            return false;
    }
}

static bool GetScanCode(DWORD vk, WORD& scan, bool& extended) {
    extended = IsExtendedKey(vk);

    struct Entry { DWORD vk; WORD scan; bool ext; };
    static constexpr Entry kTable[] = {
        { VK_LCONTROL,  0x1D, false }, { VK_RCONTROL, 0x1D, true  },
        { VK_LMENU,     0x38, false }, { VK_RMENU,    0x38, true  },
        { VK_LSHIFT,    0x2A, false }, { VK_RSHIFT,   0x36, false },
        { VK_LWIN,      0x5B, true  }, { VK_RWIN,     0x5C, true  },
        { VK_APPS,      0x5D, true  }, { VK_NUMLOCK,  0x45, true  },
        { VK_SNAPSHOT,  0x37, true  },
        { VK_NUMPAD0,   0x52, false }, { VK_NUMPAD1,  0x4F, false },
        { VK_NUMPAD2,   0x50, false }, { VK_NUMPAD3,  0x51, false },
        { VK_NUMPAD4,   0x4B, false }, { VK_NUMPAD5,  0x4C, false },
        { VK_NUMPAD6,   0x4D, false }, { VK_NUMPAD7,  0x47, false },
        { VK_NUMPAD8,   0x48, false }, { VK_NUMPAD9,  0x49, false },
        { VK_MULTIPLY,  0x37, false }, { VK_ADD,      0x4E, false },
        { VK_SUBTRACT,  0x4A, false }, { VK_DIVIDE,   0x35, true  },
        { VK_DECIMAL,   0x53, false },
    };

    for (const auto& e : kTable) {
        if (e.vk == vk) {
            scan     = e.scan;
            extended = e.ext;
            return true;
        }
    }

    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    if (!sc) sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (!sc) return false;

    extended = ((sc & 0xFF00) != 0) || extended;
    scan     = (WORD)(sc & 0xFF);
    return true;
}

static bool CanInjectVk(DWORD vk) {
    WORD s = 0; bool e = false;
    return GetScanCode(vk, s, e);
}

static bool CanRegisterHotkeyVk(UINT vk) {
    return vk != 0 && !IsModifierVk(vk);
}

// ─── Registry helper for settings ───────────────────────
static void SetStartWithWindows(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (enable) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            RegSetValueExW(hKey, L"KeyBridge", 0, REG_SZ,
                (BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"KeyBridge");
        }
        RegCloseKey(hKey);
    }
}

// ─── Injection state tracking ───────────────────────
static void TrackInjectedState(DWORD vk, bool down) {
    if (down) g_injectedDown.insert(vk);
    else       g_injectedDown.erase(vk);
}

static void FlushInjectedKeys() {
    if (g_injectedDown.empty()) return;

    std::vector<DWORD> keys(g_injectedDown.begin(), g_injectedDown.end());
    g_injectedDown.clear();

    for (DWORD vk : keys) {
        WORD scan = 0; bool ext = false;
        if (!GetScanCode(vk, scan, ext)) continue;

        INPUT inp{};
        inp.type       = INPUT_KEYBOARD;
        inp.ki.wScan   = scan;
        inp.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (ext) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &inp, sizeof(INPUT));
    }
}

// ─── AltGr helpers ──────────────────────────────────
static void ClearAltGrState() {
    if (g_hwnd) KillTimer(g_hwnd, ID_ALTGR_TIMER);
    g_altGr = {};
}

static void StartAltGrPending() {
    g_altGr = { true, false, false };
    if (g_hwnd) SetTimer(g_hwnd, ID_ALTGR_TIMER, ALTGR_DELAY_MS, nullptr);
}

// ═══════════════════════════════════════════════════
//  STATE / TRAY
// ═══════════════════════════════════════════════════
void RebuildMap() {
    g_map.clear();
    g_map.reserve(g_maps.size());
    for (const auto& m : g_maps) {
        if (m.active) g_map[m.src] = m.tgt;
    }
}

void SyncTrayTip() {
    StringCchCopyW(
        g_nid.szTip,
        ARRAYSIZE(g_nid.szTip),
        g_active ? L"KeyBridge - Active" : L"KeyBridge - Passive");
    if (g_trayOk) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void SetAppActive(bool on) {
    if (g_active == on) return;
    g_active = on;
    ClearAltGrState();
    FlushInjectedKeys();
    SyncTrayTip();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void UnregisterToggleHotkey() {
    if (g_hwnd && g_hotkeyRegistered) {
        UnregisterHotKey(g_hwnd, HK_TOGGLE);
        g_hotkeyRegistered = false;
    }
}

bool TryRegisterToggleHotkey(UINT mod, UINT vk) {
    if (!g_hwnd) return false;
    mod = NormalizeHotkeyMod(mod);
    if (!CanRegisterHotkeyVk(vk)) return false;

    if (g_hotkeyRegistered) {
        UnregisterHotKey(g_hwnd, HK_TOGGLE);
        g_hotkeyRegistered = false;
    }

    if (!RegisterHotKey(g_hwnd, HK_TOGGLE, mod, vk)) return false;

    g_hotkeyRegistered = true;
    g_hkMod = mod;
    g_hkVk  = vk;
    return true;
}

// ═══════════════════════════════════════════════════
//  CONFIG SAVE / LOAD
// ═══════════════════════════════════════════════════
bool SaveConfig() {
    const std::wstring finalPath = GetCfgPath();
    const std::wstring tmpPath   = finalPath + L".tmp";

    FILE* f = _wfopen(tmpPath.c_str(), L"w");
    if (!f) {
        MessageBoxW(g_hwnd,
            L"Failed to save configuration.\nCheck write permissions.",
            L"KeyBridge", MB_ICONWARNING);
        return false;
    }

    bool ok = true;
    ok &= (fwprintf(f, L"VERSION %d\n", CFG_VERSION) > 0);
    ok &= (fwprintf(f, L"%d\n%u %u\n", (int)g_active, g_hkMod, g_hkVk) > 0);
    ok &= (fwprintf(f, L"SETTINGS %d %d %d\n",
                    (int)g_startWithWindows,
                    (int)g_startMinimized,
                    (int)g_closeToTray) > 0);

    for (const auto& m : g_maps) {
        ok &= (fwprintf(f, L"%u %u %d\n",
                        (unsigned)m.src, (unsigned)m.tgt, (int)m.active) > 0);
    }

    if (fflush(f) != 0) ok = false;
    fclose(f);

    if (!ok) {
        DeleteFileW(tmpPath.c_str());
        MessageBoxW(g_hwnd,
            L"Config may not have saved completely.\nDisk might be full.",
            L"KeyBridge", MB_ICONWARNING);
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        MessageBoxW(g_hwnd,
            L"Failed to finalize configuration file.",
            L"KeyBridge", MB_ICONWARNING);
        return false;
    }

    return true;
}

void LoadConfig() {
    std::vector<KeyMap> tempMaps;
    std::unordered_set<DWORD> seenSrc;

    int  tempNextId = 1;
    bool tempActive = true;
    UINT tempMod    = g_hkMod;
    UINT tempVk     = g_hkVk;

    FILE* f = _wfopen(GetCfgPath().c_str(), L"r");
    if (!f) {
        RebuildMap();
        SyncTrayTip();
        return;
    }

    int  ver        = 0;
    int  activeFlag = 1;
    bool ok         = true;

    // FIX: Distinguish version mismatch from parse failure so we can back up
    // the old config before discarding it, preventing silent data loss on upgrade.
    const int verScan = fwscanf(f, L"VERSION %d\n", &ver);
    if (verScan != 1 || ver != CFG_VERSION) {
        ok = false;
        if (verScan == 1 && ver != CFG_VERSION) {
            fclose(f);
            const std::wstring cfgPath = GetCfgPath();
            const std::wstring bakPath = cfgPath + L".bak";
            CopyFileW(cfgPath.c_str(), bakPath.c_str(), FALSE);
            MessageBoxW(nullptr,
                L"KeyBridge configuration format has changed.\n\n"
                L"Your previous mappings could not be loaded.\n"
                L"A backup has been saved as keybridge.cfg.bak.",
                L"KeyBridge - Configuration Updated", MB_ICONINFORMATION);
            RebuildMap();
            SyncTrayTip();
            return;
        }
    }

    if (ok && fwscanf(f, L"%d",     &activeFlag)      != 1) ok = false;
    if (ok && fwscanf(f, L"%u %u",  &tempMod, &tempVk) != 2) ok = false;

    // Settings satırını oku (eski config'lerde yoksa atlanır)
    if (ok) {
        int sw = 0, sm = 0, ct = 0;
        if (fwscanf(f, L" SETTINGS %d %d %d", &sw, &sm, &ct) == 3) {
            g_startWithWindows = (sw != 0);
            g_startMinimized   = (sm != 0);
            g_closeToTray      = (ct != 0);
        }
    }

    if (ok) {
        tempMod = NormalizeHotkeyMod(tempMod);
        if (!CanRegisterHotkeyVk(tempVk)) tempVk = VK_F12;

        unsigned s = 0, t = 0;
        int      e = 0;

        while (fwscanf(f, L"%u %u %d", &s, &t, &e) == 3) {
            DWORD src    = (DWORD)s;
            DWORD tgt    = (DWORD)t;
            bool  active = (e == 1);

            if (!src || !tgt)                             continue;
            if (src == tgt)                               continue;
            if (seenSrc.count(src))                       continue;
            if (!CanUseAsMappingTarget(tgt))              continue;
            if (!CanInjectVk(src) || !CanInjectVk(tgt))  continue;

            seenSrc.insert(src);
            tempMaps.push_back({ tempNextId++, src, tgt, active });
        }
    }

    fclose(f);

    if (!ok) {
        RebuildMap();
        SyncTrayTip();
        return;
    }

    g_active = (activeFlag != 0);
    g_hkMod  = tempMod;
    g_hkVk   = tempVk;
    g_maps   = std::move(tempMaps);
    g_nextId = tempNextId;

    RebuildMap();
    SyncTrayTip();
}

// ═══════════════════════════════════════════════════
//  INJECTION
// ═══════════════════════════════════════════════════
bool SendRawKey(DWORD vk, bool isDown) {
    WORD scan = 0; bool ext = false;
    if (!GetScanCode(vk, scan, ext)) return false;

    INPUT inp{};
    inp.type       = INPUT_KEYBOARD;
    inp.ki.wScan   = scan;
    inp.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (ext)    inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!isDown) inp.ki.dwFlags |= KEYEVENTF_KEYUP;

    if (SendInput(1, &inp, sizeof(INPUT)) == 1) {
        TrackInjectedState(vk, isDown);
        return true;
    }
    return false;
}

bool InjectMappedKey(DWORD srcVk, bool isDown) {
    auto it = g_map.find(srcVk);
    if (it == g_map.end()) return false;

    DWORD tgtVk = it->second;
    WORD  scan  = 0;
    bool  ext   = false;
    if (!GetScanCode(tgtVk, scan, ext)) return false;

    INPUT inp{};
    inp.type       = INPUT_KEYBOARD;
    inp.ki.wScan   = scan;
    inp.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (ext)    inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!isDown) inp.ki.dwFlags |= KEYEVENTF_KEYUP;

    if (SendInput(1, &inp, sizeof(INPUT)) == 1) {
        TrackInjectedState(tgtVk, isDown);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════
//  ALTGR HANDLER
// ═══════════════════════════════════════════════════
static bool IsCtrlCandidate(const KBDLLHOOKSTRUCT* k) {
    return k &&
           (k->vkCode == VK_LCONTROL || k->vkCode == VK_CONTROL) &&
           !(k->flags & LLKHF_EXTENDED);
}

static bool IsRAltEvent(const KBDLLHOOKSTRUCT* k) {
    return k &&
           (k->vkCode == VK_RMENU || k->vkCode == VK_MENU) &&
           (k->flags & LLKHF_EXTENDED);
}

bool ProcessAltGrAwareEvent(const KBDLLHOOKSTRUCT* k, bool isDown, bool& passThrough) {
    passThrough = false;
    if (!k) return false;

    const bool ctrlCandidate = IsCtrlCandidate(k);
    const bool rAltEvent     = IsRAltEvent(k);

    if (isDown && ctrlCandidate) {
        if (g_map.count(VK_LCONTROL)) return false;

        if (!g_altGr.pending) StartAltGrPending();
        return true;
    }

    if (!g_altGr.pending) return false;

    if (isDown && rAltEvent) {
        if (!g_altGr.committed) {
            SendRawKey(VK_LCONTROL, true);
            g_altGr.committed = true;
        }
        g_altGr.altgr = true;
        if (g_hwnd) KillTimer(g_hwnd, ID_ALTGR_TIMER);
        passThrough = true;
        return true;
    }

    if (g_altGr.altgr) {
        if (!isDown && ctrlCandidate) {
            if (g_altGr.committed) SendRawKey(VK_LCONTROL, false);
            ClearAltGrState();
            return true;
        }
        passThrough = true;
        return true;
    }

    if (isDown && !ctrlCandidate && !rAltEvent) {
        if (!g_altGr.committed) {
            SendRawKey(VK_LCONTROL, true);
            g_altGr.committed = true;
            if (g_hwnd) KillTimer(g_hwnd, ID_ALTGR_TIMER);
        }
        return false;
    }

    if (!isDown && ctrlCandidate) {
        if (!g_altGr.committed) {
            SendRawKey(VK_LCONTROL, true);
            SendRawKey(VK_LCONTROL, false);
        } else {
            SendRawKey(VK_LCONTROL, false);
        }
        ClearAltGrState();
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════
//  HOOK
// ═══════════════════════════════════════════════════
LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code < 0 || g_pickActive)
        return CallNextHookEx(g_hook, code, wp, lp);

    if (wp != WM_KEYDOWN && wp != WM_SYSKEYDOWN &&
        wp != WM_KEYUP   && wp != WM_SYSKEYUP)
        return CallNextHookEx(g_hook, code, wp, lp);

    auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);

    if (k->flags & LLKHF_INJECTED)
        return CallNextHookEx(g_hook, code, wp, lp);

    const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

    if (g_active) {
        bool passThrough = false;
        if (ProcessAltGrAwareEvent(k, isDown, passThrough)) {
            return passThrough
                ? CallNextHookEx(g_hook, code, wp, lp)
                : 1;
        }

        auto it = g_map.find(k->vkCode);
        if (it != g_map.end()) {
            InjectMappedKey(k->vkCode, isDown);
            return 1;
        }
    }

    return CallNextHookEx(g_hook, code, wp, lp);
}

// ═══════════════════════════════════════════════════
//  KEY PICKER (düzeltilmiş – KEYUP tabanlı onay)
// ═══════════════════════════════════════════════════
static DWORD g_pickedKey      = 0;
static UINT  g_pickedMod      = 0;
static bool  g_pickHotkeyMode = false;
static DWORD g_candidateKey   = 0;

bool EnsurePickClassRegistered() {
    if (g_pickClassOk) return true;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = PickProc;
    wc.hInstance     = g_inst;
    wc.lpszClassName = kPickerWindowClass;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = g_curArrow ? g_curArrow : LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    g_pickClassOk = true;
    return true;
}

void FinalizePick(DWORD vk) {
    g_pickedMod = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) g_pickedMod |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) g_pickedMod |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) g_pickedMod |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
        g_pickedMod |= MOD_WIN;
    g_pickedKey = vk;
}

static std::wstring BuildPickHint(const wchar_t* title, bool hotkeyMode) {
    if (hotkeyMode)
        return L"Choose the shortcut used to switch between Active and Passive.\n\n"
               L"You can also use a single key such as F12.";

    if (title) {
        if (wcsstr(title, L"Step 1"))
            return L"Select the key you want to remap.\n\nPress any key to continue.";
        if (wcsstr(title, L"Step 2"))
            return L"Select the replacement key.\n\nPress any key to finish.";
    }
    return L"Press the key you want to capture.";
}

LRESULT CALLBACK PickProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_candidateKey = 0;
        DestroyWindow(hw);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        DWORD vk = (DWORD)wp;
        if (vk == VK_ESCAPE) {
            g_candidateKey = 0;
            DestroyWindow(hw);
            return 0;
        }

        const bool ext = (lp & (1L << 24)) != 0;

        if ((vk == VK_LCONTROL || vk == VK_CONTROL) && !ext &&
            (GetAsyncKeyState(VK_MENU) & 0x8000))
            return 0;

        UINT scanCode = (UINT)((lp >> 16) & 0xFF);
        DWORD picked  = 0;

        if (scanCode == 0x1D) {
            picked = ext ? VK_RCONTROL : VK_LCONTROL;
        } else if (scanCode == 0x38) {
            picked = ext ? VK_RMENU : VK_LMENU;
        } else {
            UINT fullScan = ext ? (0xE000 | scanCode) : scanCode;
            DWORD sp = MapVirtualKeyW(fullScan, MAPVK_VSC_TO_VK_EX);
            picked = sp ? sp : vk;
        }

        if (g_pickHotkeyMode && IsModifierVk(picked)) {
            g_candidateKey = 0;
            return 0;
        }

        g_candidateKey = picked;
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        if (g_candidateKey == 0) return 0;

        DWORD vk = (DWORD)wp;
        const bool ext = (lp & (1L << 24)) != 0;
        UINT scanCode = (UINT)((lp >> 16) & 0xFF);
        DWORD released = 0;

        if (scanCode == 0x1D) {
            released = ext ? VK_RCONTROL : VK_LCONTROL;
        } else if (scanCode == 0x38) {
            released = ext ? VK_RMENU : VK_LMENU;
        } else {
            UINT fullScan = ext ? (0xE000 | scanCode) : scanCode;
            DWORD sp = MapVirtualKeyW(fullScan, MAPVK_VSC_TO_VK_EX);
            released = sp ? sp : vk;
        }

        if (released == g_candidateKey) {
            FinalizePick(released);
            DestroyWindow(hw);
        }

        g_candidateKey = 0;
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc{};
        GetClientRect(hw, &rc);

        HBRUSH bg = CreateSolidBrush(CLR_BG);
        if (bg) { FillRect(dc, &rc, bg); DeleteObject(bg); }

        HPEN pen = CreatePen(PS_SOLID, 2, CLR_ACCENT);
        if (pen) {
            HPEN   oldPen   = (HPEN)SelectObject(dc, pen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 1, 1, rc.right - 1, rc.bottom - 1);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(pen);
        }

        SetBkMode(dc, TRANSPARENT);

        FillRoundRect(dc, 18, 18, rc.right - 36, rc.bottom - 36, 20, CLR_SURFACE);
        DrawRoundBorder(dc, 18, 18, rc.right - 36, rc.bottom - 36, 20, CLR_BORDER, 1);

        // KeyBridge logosu – üst orta
        Logo::Draw(dc, rc.right / 2 - 34, 30, 68, 68);

        // Başlık
        DrawTextModern(dc,
                       g_pickTitle.empty() ? L"Press a key" : g_pickTitle,
                       42, 110, rc.right - 84, 60,
                       CLR_TEXT, 20, true,
                       DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);

        // Açıklama kutusu
        FillRoundRect(dc, 42, 184, rc.right - 84, 120, 18, CLR_SURFACE2);
        DrawRoundBorder(dc, 42, 184, rc.right - 84, 120, 18, CLR_BORDER, 1);
        DrawTextModern(dc, g_pickHint,
                       62, 194, rc.right - 124, 100,
                       CLR_DIM, 15, false,
                       DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);

        // Alt bilgi
        DrawTextModern(dc, L"Press Esc to close",
                       rc.right / 2 - 160, rc.bottom - 50, 320, 34,
                       CLR_MUTED, 14, true,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hw, &ps);
        return 0;
    }

    case WM_DESTROY:
        g_candidateKey = 0;
        g_pickDone = true;
        return 0;
    }

    return DefWindowProcW(hw, msg, wp, lp);
}

DWORD PickKey(HWND parent, const wchar_t* title, bool hotkeyMode) {
    g_pickedKey      = 0;
    g_pickedMod      = 0;
    g_candidateKey   = 0;
    g_pickHotkeyMode = hotkeyMode;
    g_pickDone       = false;
    g_pickActive     = true;

    g_pickTitle = title ? title : L"Press a key";
    g_pickHint  = BuildPickHint(title, hotkeyMode);

    const bool hadHotkey = g_hotkeyRegistered;
    const UINT savedMod  = g_hkMod;
    const UINT savedVk   = g_hkVk;

    BOOL prevEnabled = TRUE;
    if (IsWindow(parent)) {
        prevEnabled = IsWindowEnabled(parent);
        EnableWindow(parent, FALSE);
    }

    if (hadHotkey) UnregisterToggleHotkey();

    auto Restore = [&]() {
        if (hadHotkey) TryRegisterToggleHotkey(savedMod, savedVk);
        if (IsWindow(parent)) {
            EnableWindow(parent, prevEnabled);
            SetForegroundWindow(parent);
        }
        g_pickActive = false;
    };

    if (!EnsurePickClassRegistered()) { Restore(); return 0; }

    RECT pr{};
    if (IsWindow(parent)) GetWindowRect(parent, &pr);
    const int px = pr.left + (pr.right  - pr.left  - PICK_W) / 2;
    const int py = pr.top  + (pr.bottom - pr.top   - PICK_H) / 2;

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST, kPickerWindowClass,
        title ? title : L"Press a key",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        px, py, PICK_W, PICK_H,
        parent, nullptr, g_inst, nullptr);

    if (!hw) { Restore(); return 0; }

    ApplyWindowEffects(hw);
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);
    SetForegroundWindow(hw);

    MSG msg{};
    while (true) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (r < 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_pickDone) break;
    }

    if (IsWindow(hw)) DestroyWindow(hw);
    Restore();
    return g_pickedKey;
}

// ═══════════════════════════════════════════════════
//  GDI HELPERS
// ═══════════════════════════════════════════════════
void FillRoundRect(HDC dc, int x, int y, int w, int h, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    if (!br) return;

    if (r <= 0) {
        RECT rc = { x, y, x + w, y + h };
        FillRect(dc, &rc, br);
        DeleteObject(br);
        return;
    }

    HPEN   pn = CreatePen(PS_SOLID, 0, c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN   op = (HPEN)  SelectObject(dc, pn);
    RoundRect(dc, x, y, x + w, y + h, r, r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

void DrawRoundBorder(HDC dc, int x, int y, int w, int h,
                     int r, COLORREF c, int thick) {
    HPEN pn = CreatePen(PS_SOLID, thick, c);
    if (!pn) return;

    HPEN   op = (HPEN)  SelectObject(dc, pn);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (r <= 0) Rectangle(dc, x, y, x + w, y + h);
    else        RoundRect(dc, x, y, x + w, y + h, r, r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pn);
}

void DrawTextModern(HDC dc, const std::wstring& text,
                    int x, int y, int w, int h,
                    COLORREF color,
                    int fontSize, bool bold,
                    UINT fmt) {
    if (!dc || w <= 0 || h <= 0 || text.empty()) return;

    const RECT base   = { x, y, x + w, y + h };
    const UINT dfmt   = (fmt | DT_NOPREFIX) & ~DT_CALCRECT;
    const bool wrap   = (fmt & DT_WORDBREAK) || (fmt & DT_EDITCONTROL);

    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);

    if (!wrap) {
        HFONT font = g_fc.Get(fontSize, bold);
        if (!font) return;
        HFONT old = (HFONT)SelectObject(dc, font);
        RECT  r   = base;
        DrawTextW(dc, text.c_str(), -1, &r,
                  dfmt | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        return;
    }

    for (int sz = fontSize; sz >= FontCache::kMinSz; --sz) {
        HFONT font = g_fc.Get(sz, bold);
        if (!font) continue;

        HFONT old  = (HFONT)SelectObject(dc, font);
        RECT  calc = base;
        DrawTextW(dc, text.c_str(), -1, &calc, dfmt | DT_CALCRECT);

        const bool fits = (calc.bottom - calc.top) <= h;
        if (fits || sz == FontCache::kMinSz) {
            RECT r = base;
            DrawTextW(dc, text.c_str(), -1, &r, dfmt);
            SelectObject(dc, old);
            return;
        }
        SelectObject(dc, old);
    }
}

static void AddRoundedRectPath(GraphicsPath& path, const RectF& rect, REAL radius) {
    const REAL d = radius * 2.0f;
    const REAL x = rect.X, y = rect.Y, w = rect.Width, h = rect.Height;

    path.StartFigure();
    path.AddArc(x,           y,           d, d, 180.0f, 90.0f);
    path.AddLine(x + radius, y,           x + w - radius, y);
    path.AddArc(x + w - d,   y,           d, d, 270.0f, 90.0f);
    path.AddLine(x + w,      y + radius,  x + w, y + h - radius);
    path.AddArc(x + w - d,   y + h - d,   d, d,   0.0f, 90.0f);
    path.AddLine(x + w - radius, y + h,   x + radius, y + h);
    path.AddArc(x,           y + h - d,   d, d,  90.0f, 90.0f);
    path.AddLine(x,          y + h - radius, x, y + radius);
    path.CloseFigure();
}

static void DrawToggleSwitchGdiPlus(HDC hdc, int x, int y, bool on, bool hover) {
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    const REAL trackW = 40.0f, trackH = 24.0f;
    const REAL radius = trackH / 2.0f;

    RectF       trackRect((REAL)x, (REAL)y, trackW, trackH);
    GraphicsPath trackPath;
    AddRoundedRectPath(trackPath, trackRect, radius);

    const COLORREF onTrack  = hover ? RGB(145, 130, 255) : CLR_ACCENT;
    const COLORREF offTrack = hover ? RGB(60,  61,  82)  : CLR_BORDER;

    SolidBrush trackBrush(ToGdiPlus(on ? onTrack : offTrack));
    Pen        trackPen  (ToGdiPlus(on ? CLR_ACCENT2 : CLR_MUTED), 1.0f);
    g.FillPath(&trackBrush, &trackPath);
    g.DrawPath(&trackPen,   &trackPath);

    const REAL thumbD = 20.0f;
    const REAL thumbX = on ? (x + 18.0f) : (x + 2.0f);
    const REAL thumbY = y + 2.0f;

    SolidBrush shadowBrush(Color(35, 0, 0, 0));
    g.FillEllipse(&shadowBrush, thumbX + 1.0f, thumbY + 1.0f, thumbD, thumbD);

    SolidBrush thumbBrush(Color(255, 240, 240, 255));
    Pen        thumbPen  (Color(60, 0, 0, 0), 1.0f);
    g.FillEllipse(&thumbBrush, thumbX, thumbY, thumbD, thumbD);
    g.DrawEllipse(&thumbPen,   thumbX, thumbY, thumbD, thumbD);
}

static void DrawToggleSwitchFallback(HDC dc, int x, int y, bool on, bool hover) {
    const COLORREF track  = on ? (hover ? RGB(145, 130, 255) : CLR_ACCENT)
                               : (hover ? RGB(60,  61,  82)  : CLR_BORDER);
    const COLORREF border = on ? CLR_ACCENT2 : CLR_MUTED;

    FillRoundRect (dc, x, y, 40, 24, 12, track);
    DrawRoundBorder(dc, x, y, 40, 24, 12, border, 1);

    const int thumbX = on ? x + 18 : x + 2;

    HBRUSH shadow = CreateSolidBrush(RGB(35, 35, 45));
    if (shadow) {
        HBRUSH old = (HBRUSH)SelectObject(dc, shadow);
        Ellipse(dc, thumbX + 1, y + 3, thumbX + 21, y + 23);
        SelectObject(dc, old);
        DeleteObject(shadow);
    }

    HBRUSH thumb = CreateSolidBrush(RGB(240, 240, 255));
    HPEN   pen   = CreatePen(PS_SOLID, 1, RGB(80, 80, 100));
    if (thumb && pen) {
        HBRUSH oldB = (HBRUSH)SelectObject(dc, thumb);
        HPEN   oldP = (HPEN)  SelectObject(dc, pen);
        Ellipse(dc, thumbX, y + 2, thumbX + 20, y + 22);
        SelectObject(dc, oldB);
        SelectObject(dc, oldP);
    }
    if (thumb) DeleteObject(thumb);
    if (pen)   DeleteObject(pen);
}

static void DrawToggleSwitch(HDC dc, int x, int y, bool on, bool hover) {
    if (g_gdiPlusOk) DrawToggleSwitchGdiPlus(dc, x, y, on, hover);
    else             DrawToggleSwitchFallback(dc, x, y, on, hover);
}

// ═══════════════════════════════════════════════════
//  TRAY HELPERS
// ═══════════════════════════════════════════════════
void RestoreMainWindow(HWND hw) {
    if (!IsWindow(hw)) return;
    ShowWindow(hw, SW_SHOWNORMAL);
    SetForegroundWindow(hw);
    InvalidateRect(hw, nullptr, FALSE);
}

void ShowTrayMenu(HWND hw) {
    if (!IsWindow(hw)) return;

    HMENU m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
        g_active ? L"Status: Active" : L"Status: Passive");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SHOW,      L"Open KeyBridge");
    AppendMenuW(m, MF_STRING, ID_TOGGLEAPP, L"Toggle Active / Passive");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hw);
    AllowSetForegroundWindow(ASFW_ANY);

    UINT cmd = TrackPopupMenuEx(
        m,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
        pt.x, pt.y, hw, nullptr);

    if (cmd) SendMessageW(hw, WM_COMMAND, cmd, 0);

    DestroyMenu(m);
    PostMessageW(hw, WM_NULL, 0, 0);
}

void HandleTrayEvent(HWND hw, LPARAM lp) {
    switch (lp) {
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
    // FIX: Clicking the balloon notification should restore the main window,
    // which is the natural expectation when the balloon appears after minimizing.
    case NIN_BALLOONUSERCLICK:
        RestoreMainWindow(hw);
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowTrayMenu(hw);
        break;
    default:
        break;
    }
}

// ═══════════════════════════════════════════════════
//  SCROLL
// ═══════════════════════════════════════════════════
void UpdateScroll(HWND hw) {
    RECT rc{};
    GetClientRect(hw, &rc);

    const int visibleH = std::max<int>(0, rc.bottom - LIST_TOP - FOOTER_H);
    const int n        = (int)g_maps.size();
    const int total    = n > 0 ? (n * CARD_H + (n - 1) * CARD_GAP) : 0;

    g_scrollMax = std::max(0, total - visibleH);
    g_scrollY   = std::clamp(g_scrollY, 0, g_scrollMax);
}

// ═══════════════════════════════════════════════════
//  PAINT
// ═══════════════════════════════════════════════════
void OnPaint(HWND hw) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hw, &ps);

    RECT rc{};
    GetClientRect(hw, &rc);
    const int W = rc.right;
    const int H = rc.bottom;

    if (W <= 0 || H <= 0) { EndPaint(hw, &ps); return; }

    HDC     mdc    = CreateCompatibleDC(hdc);
    HBITMAP bmp    = CreateCompatibleBitmap(hdc, W, H);
    if (!mdc || !bmp) {
        if (mdc) DeleteDC(mdc);
        if (bmp) DeleteObject(bmp);
        EndPaint(hw, &ps);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, bmp);

    HBRUSH bgBr = CreateSolidBrush(CLR_BG);
    if (bgBr) { FillRect(mdc, &rc, bgBr); DeleteObject(bgBr); }

    FillRoundRect(mdc, 0, 0, W, HEADER_H, 0, CLR_SURFACE);
    {
        HPEN lp = CreatePen(PS_SOLID, 1, CLR_BORDER);
        if (lp) {
            HPEN op = (HPEN)SelectObject(mdc, lp);
            MoveToEx(mdc, 0, HEADER_H, nullptr);
            LineTo  (mdc, W, HEADER_H);
            SelectObject(mdc, op);
            DeleteObject(lp);
        }
    }

    Logo::Draw(mdc, 20, 20, 48, 48);

    DrawTextModern(mdc, L"KeyBridge",
                   80, 18, 260, 26, CLR_TEXT, 18, true,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextModern(mdc,
        L"Universal Keyboard Remapper \u2022 Made by VryxTech",
        80, 46, 420, 18, CLR_DIM, 13, false,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Settings butonu (logo altında, solda)
    const int setW = 120, setH = 30;
    const int setX = 20, setY = 76;
    bool setHov = (g_hover == -5);
    FillRoundRect (mdc, setX, setY, setW, setH, 8, setHov ? RGB(60,60,70) : CLR_SURFACE2);
    DrawRoundBorder(mdc, setX, setY, setW, setH, 8, CLR_BORDER);
    DrawTextModern(mdc, L"Settings",
                   setX, setY, setW, setH, CLR_TEXT, 13, true,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const bool     act      = g_active;
    const COLORREF badgeClr = act ? CLR_GREEN : CLR_RED;
    const COLORREF badgeBg  = act ? RGB(15, 40, 22) : RGB(45, 15, 15);
    const int bx = W - 145, by = 24, bw = 125, bh = 34;

    FillRoundRect (mdc, bx, by, bw, bh, 17, badgeBg);
    DrawRoundBorder(mdc, bx, by, bw, bh, 17, badgeClr);

    {
        HBRUSH db = CreateSolidBrush(badgeClr);
        HPEN   dp = CreatePen(PS_SOLID, 0, badgeClr);
        if (db && dp) {
            HBRUSH odb = (HBRUSH)SelectObject(mdc, db);
            HPEN   odp = (HPEN)  SelectObject(mdc, dp);
            Ellipse(mdc, bx + 12, by + 11, bx + 24, by + 23);
            SelectObject(mdc, odb);
            SelectObject(mdc, odp);
        }
        if (db) DeleteObject(db);
        if (dp) DeleteObject(dp);
    }

    DrawTextModern(mdc, act ? L"ACTIVE" : L"PASSIVE",
                   bx + 28, by, bw - 32, bh, badgeClr, 13, true);

    bool hkHov = (g_hover == -4 && g_hBtn == 1);
    std::wstring hkStr = L"[" + GetHotkeyString() + L"] toggle";
    DrawTextModern(mdc, hkStr,
                   bx - 148, by + 4, 140, 26,
                   hkHov ? CLR_ACCENT : CLR_DIM, 13, false,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // "Clear All" button
    const int clrW = 156;
    const int clrH = 30;
    const int clrX = W - 20 - 156 - 10 - clrW;
    const int clrY = MulDiv(76, g_dpi, 96);
    bool clrHov = (g_hover == -3);
    FillRoundRect (mdc, clrX, clrY, clrW, clrH, 9, clrHov ? CLR_RED : RGB(50, 15, 15));
    DrawRoundBorder(mdc, clrX, clrY, clrW, clrH, 9, CLR_RED);
    DrawTextModern(mdc, L"Clear All",
                   clrX, clrY, clrW, clrH, CLR_TEXT, 13, true,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // "New Mapping" button
    const int nw = 156, nh = 30, nx = W - 20 - nw, ny = MulDiv(76, g_dpi, 96);
    bool newHov = (g_hover == -2);
    FillRoundRect (mdc, nx, ny, nw, nh, 9, newHov ? CLR_ACCENT : CLR_ACCENT2);
    DrawRoundBorder(mdc, nx, ny, nw, nh, 9, CLR_ACCENT);
    DrawTextModern(mdc, L"+ New Mapping",
                   nx + 16, ny, nw - 16, nh, CLR_TEXT, 13, true,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t hdr[64];
    StringCchPrintfW(hdr, 64, L"Mappings  (%d)", (int)g_maps.size());
    DrawTextModern(mdc, hdr,
                   20, HEADER_H + 4, 180, 20, CLR_DIM, 12, true,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    HRGN clip = CreateRectRgn(0, LIST_TOP, W, H - FOOTER_H);
    if (clip) SelectClipRgn(mdc, clip);

    if (g_maps.empty()) {
        SelectClipRgn(mdc, nullptr);
        const int ex = W / 2 - 210, ey = LIST_TOP + 48, ew = 420, eh = 108;
        FillRoundRect (mdc, ex, ey, ew, eh, 14, CLR_SURFACE);
        DrawRoundBorder(mdc, ex, ey, ew, eh, 14, CLR_BORDER);
        DrawTextModern(mdc, L"No remaps yet",
                       ex, ey + 16, ew, 24, CLR_TEXT, 17, true);
        DrawTextModern(mdc,
            L"Click \"New Mapping\" to choose a key you press and the key it should become.",
            ex + 18, ey + 46, ew - 36, 42, CLR_DIM, 13, false,
            DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);
    } else {
        for (int i = 0; i < (int)g_maps.size(); i++) {
            const auto& m  = g_maps[i];
            CardLayout  cl = CardLayout::Compute(W, i, g_scrollY);

            if (cl.cardY + CARD_H < LIST_TOP || cl.cardY > H) continue;

            const bool hov = (g_hover == i);

            FillRoundRect(mdc, cl.cardX, cl.cardY, cl.cardW, CARD_H, 12,
                          hov ? RGB(32, 34, 48) : CLR_SURFACE);
            DrawRoundBorder(mdc, cl.cardX, cl.cardY, cl.cardW, CARD_H, 12,
                            m.active ? (hov ? CLR_ACCENT : CLR_BORDER) : CLR_BORDER,
                            hov ? 2 : 1);

            if (m.active) {
                HBRUSH sb = CreateSolidBrush(CLR_ACCENT);
                if (sb) {
                    RECT sr = { cl.cardX + 1, cl.cardY + 12,
                                cl.cardX + 4, cl.cardY + CARD_H - 12 };
                    HBRUSH osb = (HBRUSH)SelectObject(mdc, sb);
                    FillRect(mdc, &sr, sb);
                    SelectObject(mdc, osb);
                    DeleteObject(sb);
                }
            }

            DrawTextModern(mdc, L"FROM",
                           cl.srcX, cl.cardY + 8, cl.chipW, 14,
                           CLR_DIM, 10, true,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextModern(mdc, L"TO",
                           cl.tgtX, cl.cardY + 8, cl.chipW, 14,
                           CLR_DIM, 10, true,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            FillRoundRect (mdc, cl.srcX, cl.srcY, cl.chipW, cl.chipH, 8, CLR_SURFACE2);
            DrawRoundBorder(mdc, cl.srcX, cl.srcY, cl.chipW, cl.chipH, 8, CLR_BORDER);
            DrawTextModern(mdc, GetKeyName(m.src),
                           cl.srcX + 4, cl.srcY, cl.chipW - 8, cl.chipH,
                           CLR_TEXT, 13, true,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            DrawTextModern(mdc, L"\u2192",
                           cl.arrowX, cl.srcY - 1, 28, cl.chipH + 2,
                           CLR_ACCENT, 16, true,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            FillRoundRect (mdc, cl.tgtX, cl.tgtY, cl.chipW, cl.chipH, 8, CLR_SURFACE2);
            DrawRoundBorder(mdc, cl.tgtX, cl.tgtY, cl.chipW, cl.chipH, 8, CLR_BORDER);
            DrawTextModern(mdc, GetKeyName(m.tgt),
                           cl.tgtX + 4, cl.tgtY, cl.chipW - 8, cl.chipH,
                           CLR_TEXT, 13, true,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            DrawToggleSwitch(mdc, cl.togX, cl.togY, m.active,
                             g_hover == i && g_hBtn == 0);

            const bool delHov = (g_hover == i && g_hBtn == 1);
            if (delHov) FillRoundRect(mdc, cl.delX, cl.delY, 28, 28, 6, RGB(50, 15, 15));
            DrawTextModern(mdc, L"\u00D7",
                           cl.delX, cl.delY, 28, 28,
                           delHov ? CLR_RED : CLR_MUTED, 14, true);
        }
    }

    SelectClipRgn(mdc, nullptr);
    if (clip) DeleteObject(clip);

    if (g_scrollMax > 0) {
        const int trackH = H - LIST_TOP - FOOTER_H;
        const int thumbH = std::max(20, trackH * trackH / (trackH + g_scrollMax));
        const int thumbY = LIST_TOP +
            (int)((long long)g_scrollY * (trackH - thumbH) / g_scrollMax);
        FillRoundRect(mdc, W - 7, LIST_TOP, 4, trackH, 2, CLR_MUTED);
        FillRoundRect(mdc, W - 7, thumbY,   4, thumbH, 2, CLR_ACCENT);
    }

    {
        HPEN fp = CreatePen(PS_SOLID, 1, CLR_BORDER);
        if (fp) {
            HPEN ofp = (HPEN)SelectObject(mdc, fp);
            MoveToEx(mdc, 0, H - FOOTER_H, nullptr);
            LineTo  (mdc, W, H - FOOTER_H);
            SelectObject(mdc, ofp);
            DeleteObject(fp);
        }
    }

    // FIX: Footer text reflects the actual close behavior based on current setting.
    // When g_closeToTray is false the window close button exits the application,
    // so displaying "Close to tray" would actively mislead the user.
    const wchar_t* footerText = g_closeToTray
        ? L"KeyBridge \u2022 Close to tray \u2022 Double-click tray icon to open"
        : L"KeyBridge \u2022 Close exits the application";
    DrawTextModern(mdc, footerText,
        0, H - FOOTER_H, W, FOOTER_H, CLR_MUTED, 12, false);

    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
    EndPaint(hw, &ps);
}

// ═══════════════════════════════════════════════════
//  HIT TEST
// ═══════════════════════════════════════════════════
HitResult HitTest(HWND hw, int mx, int my) {
    RECT rc{};
    GetClientRect(hw, &rc);
    const int W = rc.right;

    if (mx >= W - 145 && mx <= W - 20  && my >= 24 && my <= 58) return { -4, 0 };
    if (mx >= W - 286 && mx <= W - 150 && my >= 28 && my <= 54) return { -4, 1 };

    // Settings butonu (header içinde, logo altı)
    const int setW = 120, setH = 30;
    const int setX = 20, setY = 76;
    if (mx >= setX && mx <= setX + setW && my >= setY && my <= setY + setH)
        return { -5, -1 };

    const int clrW = 156;
    const int clrH = 30;
    const int clrX = W - 20 - 156 - 10 - clrW;
    const int clrY = MulDiv(76, g_dpi, 96);
    if (mx >= clrX && mx <= clrX + clrW && my >= clrY && my <= clrY + clrH)
        return { -3, -1 };

    const int nx = W - 20 - 156;
    if (mx >= nx && mx <= nx + 156 && my >= MulDiv(76, g_dpi, 96) && my <= MulDiv(76, g_dpi, 96) + 30)
        return { -2, -1 };

    if (my < LIST_TOP) return { -1, -1 };

    const int ly  = my + g_scrollY - LIST_TOP;
    const int idx = ly / (CARD_H + CARD_GAP);
    const int rem = ly % (CARD_H + CARD_GAP);

    if (rem < 0 || rem >= CARD_H || idx < 0 || idx >= (int)g_maps.size())
        return { -1, -1 };

    CardLayout cl = CardLayout::Compute(W, idx, g_scrollY);

    if (mx >= cl.togX && mx <= cl.togX + 40 &&
        my >= cl.togY && my <= cl.togY + 24) return { idx, 0 };

    if (mx >= cl.delX && mx <= cl.delX + 28 &&
        my >= cl.delY && my <= cl.delY + 28) return { idx, 1 };

    return { idx, -1 };
}

// ═══════════════════════════════════════════════════
//  SETTINGS DIALOG
// ═══════════════════════════════════════════════════
static bool g_dlgStartWithWindows = false;
static bool g_dlgStartMinimized   = false;
static bool g_dlgCloseToTray      = false;

struct SettingRow {
    const wchar_t* label;
    bool*          value;
};

static const SettingRow g_settingRows[] = {
    { L"Start with Windows",           &g_dlgStartWithWindows },
    { L"Start minimized to tray",      &g_dlgStartMinimized   },
    { L"Close button minimizes to tray", &g_dlgCloseToTray    },
};
static constexpr int kRowCount = sizeof(g_settingRows) / sizeof(g_settingRows[0]);

// Settings dialog layout constants (DPI‑scaled where used)
static constexpr int kSettingsBaseW     = 420;
static constexpr int kSettingsHeaderH   = 60;
static constexpr int kSettingsRowH      = 52;
static constexpr int kSettingsFooterH   = 72;
static constexpr int kSettingsBottomPad = 30;

static void GetSettingRowRect(HWND hw, int rowIdx, RECT& outRect) {
    RECT rc;
    GetClientRect(hw, &rc);
    const int headerH = MulDiv(kSettingsHeaderH, g_dpi, 96);
    const int rowH    = MulDiv(kSettingsRowH, g_dpi, 96);
    const int toggleW = MulDiv(40, g_dpi, 96);
    const int toggleH = MulDiv(24, g_dpi, 96);
    const int paddingX = MulDiv(24, g_dpi, 96);

    int y = headerH + rowIdx * rowH;
    int toggleX = rc.right - paddingX - toggleW;
    int toggleY = y + (rowH - toggleH) / 2;

    outRect.left   = toggleX;
    outRect.top    = toggleY;
    outRect.right  = toggleX + toggleW;
    outRect.bottom = toggleY + toggleH;
}

LRESULT CALLBACK SettingsDlgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetAppDpi(hw);
        return 0;

    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(hw, nullptr, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetAppDpi(hw);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hw);
            return 0;
        }
        if (wp == VK_RETURN) {
            g_startWithWindows = g_dlgStartWithWindows;
            g_startMinimized   = g_dlgStartMinimized;
            g_closeToTray      = g_dlgCloseToTray;
            SetStartWithWindows(g_startWithWindows);
            SaveConfig();
            DestroyWindow(hw);
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hw, &rc);
        const int footerH = MulDiv(kSettingsFooterH, g_dpi, 96);
        const int btnW = MulDiv(80, g_dpi, 96);
        const int btnH = MulDiv(28, g_dpi, 96);
        const int btnY = rc.bottom - footerH + (footerH - btnH) / 2;
        RECT okRect  = { rc.right - MulDiv(180, g_dpi, 96), btnY,
                         rc.right - MulDiv(180, g_dpi, 96) + btnW, btnY + btnH };
        RECT cancelRect = { rc.right - MulDiv(90, g_dpi, 96), btnY,
                            rc.right - MulDiv(90, g_dpi, 96) + btnW, btnY + btnH };

        int old = g_settingsHoverBtn;
        if (PtInRect(&okRect, pt))      g_settingsHoverBtn = 1;
        else if (PtInRect(&cancelRect, pt)) g_settingsHoverBtn = 2;
        else                            g_settingsHoverBtn = 0;

        if (old != g_settingsHoverBtn)  InvalidateRect(hw, nullptr, FALSE);

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hw, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_settingsHoverBtn != 0) {
            g_settingsHoverBtn = 0;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP: {
        int mx = GET_X_LPARAM(lp);
        int my = GET_Y_LPARAM(lp);
        for (int i = 0; i < kRowCount; ++i) {
            RECT tr;
            GetSettingRowRect(hw, i, tr);
            if (mx >= tr.left && mx <= tr.right && my >= tr.top && my <= tr.bottom) {
                *g_settingRows[i].value = !*g_settingRows[i].value;
                InvalidateRect(hw, nullptr, FALSE);
                return 0;
            }
        }

        RECT rc;
        GetClientRect(hw, &rc);
        const int footerH = MulDiv(kSettingsFooterH, g_dpi, 96);
        const int btnW = MulDiv(80, g_dpi, 96);
        const int btnH = MulDiv(28, g_dpi, 96);
        const int btnY = rc.bottom - footerH + (footerH - btnH) / 2;
        RECT okRect  = { rc.right - MulDiv(180, g_dpi, 96), btnY,
                         rc.right - MulDiv(180, g_dpi, 96) + btnW, btnY + btnH };
        RECT cancelRect = { rc.right - MulDiv(90, g_dpi, 96), btnY,
                            rc.right - MulDiv(90, g_dpi, 96) + btnW, btnY + btnH };

        if (mx >= okRect.left && mx <= okRect.right && my >= okRect.top && my <= okRect.bottom) {
            g_startWithWindows = g_dlgStartWithWindows;
            g_startMinimized   = g_dlgStartMinimized;
            g_closeToTray      = g_dlgCloseToTray;
            SetStartWithWindows(g_startWithWindows);
            SaveConfig();
            DestroyWindow(hw);
            return 0;
        }
        if (mx >= cancelRect.left && mx <= cancelRect.right && my >= cancelRect.top && my <= cancelRect.bottom) {
            DestroyWindow(hw);
            return 0;
        }
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hw, &pt);

        for (int i = 0; i < kRowCount; ++i) {
            RECT tr;
            GetSettingRowRect(hw, i, tr);
            if (PtInRect(&tr, pt)) {
                SetCursor(g_curHand ? g_curHand : LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }

        RECT rc;
        GetClientRect(hw, &rc);
        const int footerH = MulDiv(kSettingsFooterH, g_dpi, 96);
        const int btnW = MulDiv(80, g_dpi, 96);
        const int btnH = MulDiv(28, g_dpi, 96);
        const int btnY = rc.bottom - footerH + (footerH - btnH) / 2;
        RECT okRect  = { rc.right - MulDiv(180, g_dpi, 96), btnY,
                         rc.right - MulDiv(180, g_dpi, 96) + btnW, btnY + btnH };
        RECT cancelRect = { rc.right - MulDiv(90, g_dpi, 96), btnY,
                            rc.right - MulDiv(90, g_dpi, 96) + btnW, btnY + btnH };

        if (PtInRect(&okRect, pt) || PtInRect(&cancelRect, pt)) {
            SetCursor(g_curHand ? g_curHand : LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }

        SetCursor(g_curArrow ? g_curArrow : LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);

        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, bmp);

        HBRUSH bg = CreateSolidBrush(CLR_BG);
        FillRect(mdc, &rc, bg);
        DeleteObject(bg);

        const int headerH = MulDiv(kSettingsHeaderH, g_dpi, 96);
        RECT hdrRect = { 0, 0, rc.right, headerH };
        HBRUSH hdrBr = CreateSolidBrush(CLR_SURFACE);
        FillRect(mdc, &hdrRect, hdrBr);
        DeleteObject(hdrBr);

        SetBkMode(mdc, TRANSPARENT);
        DrawTextModern(mdc, L"Settings",
                       MulDiv(24, g_dpi, 96), 0,
                       rc.right - MulDiv(48, g_dpi, 96), headerH,
                       CLR_TEXT, 18, true,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        const int rowH = MulDiv(kSettingsRowH, g_dpi, 96);
        for (int i = 0; i < kRowCount; ++i) {
            int y = headerH + i * rowH;
            DrawTextModern(mdc, g_settingRows[i].label,
                           MulDiv(24, g_dpi, 96), y,
                           rc.right - MulDiv(120, g_dpi, 96), rowH,
                           CLR_TEXT, 16, false,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT tr;
            GetSettingRowRect(hw, i, tr);
            DrawToggleSwitch(mdc, tr.left, tr.top, *g_settingRows[i].value, false);
        }

        const int footerH = MulDiv(kSettingsFooterH, g_dpi, 96);
        RECT footRect = { 0, rc.bottom - footerH, rc.right, rc.bottom };
        HBRUSH footBr = CreateSolidBrush(CLR_SURFACE);
        FillRect(mdc, &footRect, footBr);
        DeleteObject(footBr);

        const int btnW = MulDiv(80, g_dpi, 96);
        const int btnH = MulDiv(28, g_dpi, 96);
        const int btnY = rc.bottom - footerH + (footerH - btnH) / 2;

        // OK butonu
        RECT okR = { rc.right - MulDiv(180, g_dpi, 96), btnY,
                     rc.right - MulDiv(180, g_dpi, 96) + btnW, btnY + btnH };
        COLORREF okBg = (g_settingsHoverBtn == 1) ? CLR_ACCENT : CLR_ACCENT2;
        FillRoundRect(mdc, okR.left, okR.top, okR.right - okR.left, okR.bottom - okR.top,
                      6, okBg);
        DrawRoundBorder(mdc, okR.left, okR.top, okR.right - okR.left, okR.bottom - okR.top,
                        6, CLR_ACCENT);
        DrawTextModern(mdc, L"OK", okR.left, okR.top,
                       okR.right - okR.left, okR.bottom - okR.top,
                       CLR_TEXT, 14, true);

        // Cancel butonu
        RECT cancelR = { rc.right - MulDiv(90, g_dpi, 96), btnY,
                         rc.right - MulDiv(90, g_dpi, 96) + btnW, btnY + btnH };
        COLORREF cancelBg = (g_settingsHoverBtn == 2) ? RGB(50, 50, 60) : CLR_SURFACE2;
        FillRoundRect(mdc, cancelR.left, cancelR.top,
                      cancelR.right - cancelR.left, cancelR.bottom - cancelR.top,
                      6, cancelBg);
        DrawRoundBorder(mdc, cancelR.left, cancelR.top,
                        cancelR.right - cancelR.left, cancelR.bottom - cancelR.top,
                        6, CLR_BORDER);
        DrawTextModern(mdc, L"Cancel", cancelR.left, cancelR.top,
                       cancelR.right - cancelR.left, cancelR.bottom - cancelR.top,
                       CLR_DIM, 14, false);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;

    case WM_DESTROY:
        g_settingsHoverBtn = 0;
        g_hwndSettings = nullptr;
        if (g_hwnd && IsWindow(g_hwnd)) {
            EnableWindow(g_hwnd, TRUE);
            SetForegroundWindow(g_hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

bool ShowSettingsDialog(HWND parent) {
    if (g_hwndSettings && IsWindow(g_hwndSettings)) {
        SetForegroundWindow(g_hwndSettings);
        return true;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = SettingsDlgProc;
        wc.hInstance     = g_inst;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"KeyBridgeSettings";
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        classRegistered = true;
    }

    g_dlgStartWithWindows = g_startWithWindows;
    g_dlgStartMinimized   = g_startMinimized;
    g_dlgCloseToTray      = g_closeToTray;

    EnableWindow(parent, FALSE);

    const int baseW = kSettingsBaseW;
    const int headerH = kSettingsHeaderH;
    const int rowH = kSettingsRowH;
    const int footerH = kSettingsFooterH;
    const int bottomPadding = kSettingsBottomPad;
    int dlgW = MulDiv(baseW, g_dpi, 96);
    int dlgH = MulDiv(headerH + kRowCount * rowH + bottomPadding + footerH, g_dpi, 96);

    RECT pr;
    GetWindowRect(parent, &pr);
    int px = pr.left + (pr.right - pr.left - dlgW) / 2;
    int py = pr.top  + (pr.bottom - pr.top - dlgH) / 2;

    g_hwndSettings = CreateWindowExW(0, L"KeyBridgeSettings", L"Settings",
                                     WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                     px, py, dlgW, dlgH,
                                     parent, nullptr, g_inst, nullptr);
    if (!g_hwndSettings) {
        EnableWindow(parent, TRUE);
        return false;
    }

    ApplyWindowEffects(g_hwndSettings);
    ShowWindow(g_hwndSettings, SW_SHOW);
    UpdateWindow(g_hwndSettings);
    SetForegroundWindow(g_hwndSettings);
    return true;
}

// ═══════════════════════════════════════════════════
//  WINDOW PROC
// ═══════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = MIN_W;
        mmi->ptMinTrackSize.y = MIN_H;
        return 0;
    }

    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(hw, nullptr,
            r->left, r->top,
            r->right - r->left, r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        SetAppDpi(hw);
        UpdateScroll(hw);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wp == ID_ALTGR_TIMER) {
            if (g_altGr.pending && !g_altGr.committed && !g_altGr.altgr) {
                SendRawKey(VK_LCONTROL, true);
                g_altGr.committed = true;
            }
            KillTimer(hw, ID_ALTGR_TIMER);
            return 0;
        }
        break;

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hw, &pt);
            HitResult h = HitTest(hw, pt.x, pt.y);
            const bool hand = (h.card == -2 || h.card == -3 || h.card == -4 ||
                               h.card == -5 || (h.card >= 0 && h.btn != -1));
            SetCursor(hand
                ? (g_curHand  ? g_curHand  : LoadCursorW(nullptr, IDC_HAND))
                : (g_curArrow ? g_curArrow : LoadCursorW(nullptr, IDC_ARROW)));
            return TRUE;
        }
        break;
    }

    case WM_PAINT:
        OnPaint(hw);
        return 0;

    case WM_SIZE:
        UpdateScroll(hw);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        g_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);

        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL) lines = 5;

        const int step = (int)lines * (CARD_H + CARD_GAP);

        while (g_wheelRemainder >=  WHEEL_DELTA) {
            g_scrollY -= step;
            g_wheelRemainder -= WHEEL_DELTA;
        }
        while (g_wheelRemainder <= -WHEEL_DELTA) {
            g_scrollY += step;
            g_wheelRemainder += WHEEL_DELTA;
        }

        g_scrollY = std::clamp(g_scrollY, 0, g_scrollMax);
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (g_pickActive) return 0;

        RECT rc;
        GetClientRect(hw, &rc);
        const int W = rc.right;
        const int H = rc.bottom;

        const int trackX = W - 7;
        const int trackW = 4;
        const int trackTop = LIST_TOP;
        const int trackBottom = H - FOOTER_H;
        const int trackH = trackBottom - trackTop;

        if (trackH <= 0) break;

        const int mx = GET_X_LPARAM(lp);
        const int my = GET_Y_LPARAM(lp);

        if (mx < trackX || mx > trackX + trackW ||
            my < trackTop || my > trackBottom)
            break;

        const int thumbH = std::max(20, trackH * trackH / (trackH + g_scrollMax));
        const int maxThumbY = trackBottom - thumbH;
        const int thumbY = (g_scrollMax > 0)
            ? trackTop + (int)((long long)g_scrollY * (trackH - thumbH) / g_scrollMax)
            : trackTop;

        if (my >= thumbY && my <= thumbY + thumbH) {
            g_scrollDragging = true;
            g_scrollDragOffset = my - thumbY;
            SetCapture(hw);
        } else {
            int targetY = my - thumbH / 2;
            targetY = std::clamp(targetY, trackTop, maxThumbY);
            if (trackH - thumbH > 0)
                g_scrollY = (int)((long long)(targetY - trackTop) * g_scrollMax / (trackH - thumbH));
            else
                g_scrollY = 0;
            g_scrollY = std::clamp(g_scrollY, 0, g_scrollMax);
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    }

    // FIX: Handle WM_CAPTURECHANGED to clear scrollbar drag state when the
    // mouse capture is released externally (Alt+Tab, UAC prompt, etc.).
    // Without this, g_scrollDragging stays true and the next WM_MOUSEMOVE
    // causes the scroll position to jump erratically.
    case WM_CAPTURECHANGED:
        if (g_scrollDragging) {
            g_scrollDragging = false;
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (g_scrollDragging) {
            RECT rc;
            GetClientRect(hw, &rc);
            const int H = rc.bottom;
            const int trackTop = LIST_TOP;
            const int trackBottom = H - FOOTER_H;
            const int trackH = trackBottom - trackTop;
            const int thumbH = std::max(20, trackH * trackH / (trackH + g_scrollMax));
            const int maxThumbY = trackBottom - thumbH;

            int my = GET_Y_LPARAM(lp);
            int newThumbY = my - g_scrollDragOffset;
            newThumbY = std::clamp(newThumbY, trackTop, maxThumbY);

            if (trackH - thumbH > 0)
                g_scrollY = (int)((long long)(newThumbY - trackTop) * g_scrollMax / (trackH - thumbH));
            else
                g_scrollY = 0;
            g_scrollY = std::clamp(g_scrollY, 0, g_scrollMax);

            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }

        HitResult h = HitTest(hw, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h.card != g_hover || h.btn != g_hBtn) {
            g_hover = h.card;
            g_hBtn  = h.btn;
            InvalidateRect(hw, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hw, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hover = -1;
        g_hBtn  = -1;
        InvalidateRect(hw, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        if (g_pickActive) return 0;

        if (g_scrollDragging) {
            g_scrollDragging = false;
            ReleaseCapture();
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }

        HitResult h = HitTest(hw, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (h.card == -4) {
            if (h.btn == 0) {
                SetAppActive(!g_active);
                SaveConfig();
                InvalidateRect(hw, nullptr, FALSE);
            } else if (h.btn == 1) {
                const UINT oldMod = g_hkMod;
                const UINT oldVk  = g_hkVk;
                const bool wasReg = g_hotkeyRegistered;

                if (wasReg) UnregisterToggleHotkey();

                DWORD picked = PickKey(hw,
                    L"Choose the shortcut that toggles Active / Passive", true);

                if (picked) {
                    const UINT newMod = NormalizeHotkeyMod(g_pickedMod);
                    const UINT newVk  = g_pickedKey;

                    // FIX: Warn the user before accepting a bare printable key
                    // (letter, digit, OEM punctuation, etc.) with no modifier as
                    // the toggle hotkey. Such keys are consumed system-wide by
                    // RegisterHotKey, making normal typing impossible until changed.
                    // Function keys (F1-F24) are intentionally excluded from this
                    // check as they are safe and common bare-key choices.
                    if (newMod == 0) {
                        const bool isBareAlpha = (newVk >= 'A' && newVk <= 'Z');
                        const bool isBareDigit = (newVk >= '0' && newVk <= '9');
                        const bool isBarePunct = (newVk >= 0xBA && newVk <= 0xDF);
                        const bool isBareTyping = (newVk == VK_SPACE ||
                                                   newVk == VK_TAB   ||
                                                   newVk == VK_RETURN ||
                                                   newVk == VK_BACK);
                        if (isBareAlpha || isBareDigit || isBarePunct || isBareTyping) {
                            if (MessageBoxW(hw,
                                L"Choosing a plain letter or number as your toggle shortcut will "
                                L"block that key from working in all applications while KeyBridge is active.\n\n"
                                L"Are you sure?",
                                L"KeyBridge", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                            {
                                if (wasReg && !TryRegisterToggleHotkey(oldMod, oldVk)) {
                                    MessageBoxW(hw,
                                        L"Failed to restore the previous hotkey.",
                                        L"KeyBridge", MB_ICONERROR);
                                }
                                InvalidateRect(hw, nullptr, FALSE);
                                return 0;
                            }
                        }
                    }

                    if (TryRegisterToggleHotkey(newMod, newVk)) {
                        SaveConfig();
                    } else {
                        if (!TryRegisterToggleHotkey(oldMod, oldVk)) {
                            MessageBoxW(hw,
                                L"Failed to restore the previous hotkey.",
                                L"KeyBridge", MB_ICONERROR);
                        }
                        MessageBoxW(hw,
                            L"That shortcut is already in use by another app.\n\n"
                            L"Please choose a different toggle shortcut.",
                            L"KeyBridge", MB_ICONWARNING);
                    }
                    InvalidateRect(hw, nullptr, FALSE);
                } else {
                    if (wasReg && !TryRegisterToggleHotkey(oldMod, oldVk)) {
                        MessageBoxW(hw,
                            L"Failed to restore the previous hotkey.",
                            L"KeyBridge", MB_ICONERROR);
                    }
                }
            }
            return 0;
        }

        if (h.card == -5) {
            ShowSettingsDialog(hw);
            return 0;
        }

        if (h.card == -3) {
            if (MessageBoxW(hw,
                L"Are you sure you want to remove ALL remappings?",
                L"KeyBridge - Clear All",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            {
                g_maps.clear();
                g_hover = -1;
                g_hBtn  = -1;
                RebuildMap();
                UpdateScroll(hw);
                SaveConfig();
                InvalidateRect(hw, nullptr, FALSE);
            }
            return 0;
        }

        if (h.card == -2) {
            DWORD src = PickKey(hw, L"Step 1 of 2 — choose the key you press", false);
            if (!src) return 0;

            auto existing = std::find_if(g_maps.begin(), g_maps.end(),
                [src](const KeyMap& m) { return m.src == src; });

            if (existing != g_maps.end()) {
                MessageBoxW(hw,
                    (L"That key is already mapped.\n\nKey: " +
                     GetKeyName(src) +
                     L"\n\nPlease delete the old mapping first.").c_str(),
                    L"KeyBridge", MB_ICONINFORMATION);
                return 0;
            }

            DWORD tgt = PickKey(hw, L"Step 2 of 2 — choose the key it should become", false);
            if (!tgt) return 0;

            // FIX: Reject src == tgt before creating the mapping. Without this
            // check, the mapping is accepted at runtime but silently discarded
            // by LoadConfig on the next launch, causing the mapping to disappear
            // without explanation.
            if (tgt == src) {
                MessageBoxW(hw,
                    L"The source and target keys must be different.",
                    L"KeyBridge", MB_ICONINFORMATION);
                return 0;
            }

            if (!CanInjectVk(src) || !CanInjectVk(tgt)) {
                MessageBoxW(hw,
                    L"One of the selected keys cannot be injected reliably on this system.\n\n"
                    L"Please choose another pair.",
                    L"KeyBridge", MB_ICONWARNING);
                return 0;
            }

            g_maps.push_back({ g_nextId++, src, tgt, true });
            RebuildMap();
            UpdateScroll(hw);
            SaveConfig();
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }

        if (h.card >= 0 && h.card < (int)g_maps.size()) {
            if (h.btn == 0) {
                g_maps[h.card].active = !g_maps[h.card].active;
                RebuildMap();
                SaveConfig();
                InvalidateRect(hw, nullptr, FALSE);
            } else if (h.btn == 1) {
                std::wstring q =
                    L"Delete this mapping?\n\nFROM: " +
                    GetKeyName(g_maps[h.card].src) +
                    L"\nTO:   " +
                    GetKeyName(g_maps[h.card].tgt);

                if (MessageBoxW(hw, q.c_str(),
                                L"Confirm Delete", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    g_maps.erase(g_maps.begin() + h.card);
                    g_hover = -1;
                    g_hBtn  = -1;
                    RebuildMap();
                    UpdateScroll(hw);
                    SaveConfig();
                    InvalidateRect(hw, nullptr, FALSE);
                }
            }
        }
        return 0;
    }

    case WM_HOTKEY:
        if (wp == HK_TOGGLE) {
            SetAppActive(!g_active);
            SaveConfig();
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;

    case WM_TRAY:
        HandleTrayEvent(hw, lp);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SHOW:
            RestoreMainWindow(hw);
            return 0;
        case ID_TOGGLEAPP:
            SetAppActive(!g_active);
            SaveConfig();
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        case ID_EXIT:
            SaveConfig();
            DestroyWindow(hw);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        if (g_hwndSettings && IsWindow(g_hwndSettings)) {
            DestroyWindow(g_hwndSettings);
        }
        if (g_trayOk && g_closeToTray) {
            ShowWindowAsync(hw, SW_HIDE);
            if (!g_trayTipShown) {
                g_trayTipShown = true;
                g_nid.uFlags |= NIF_INFO;
                StringCchCopyW(g_nid.szInfoTitle, ARRAYSIZE(g_nid.szInfoTitle),
                               L"KeyBridge is still running");
                StringCchCopyW(g_nid.szInfo, ARRAYSIZE(g_nid.szInfo),
                               L"Minimized to the system tray.\n"
                               L"Double-click the tray icon to open it again.");
                g_nid.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                g_nid.uFlags &= ~NIF_INFO;
            }
            return 0;
        }
        DestroyWindow(hw);
        return 0;

    case WM_DESTROY:
        ClearAltGrState();
        FlushInjectedKeys();

        if (g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
        }

        if (g_trayOk) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            g_trayOk = false;
        }

        UnregisterToggleHotkey();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hw, msg, wp, lp);
}

// ═══════════════════════════════════════════════════
//  WINMAIN
// ═══════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int nShow) {
    g_inst = hi;

    SetDpiAwareness();
    g_gdiPlusOk = StartGdiPlus();

    if (g_gdiPlusOk) {
        Logo::Initialize();
    }

    g_curArrow = LoadCursorW(nullptr, IDC_ARROW);
    g_curHand  = LoadCursorW(nullptr, IDC_HAND);

    HANDLE mx = CreateMutexW(nullptr, TRUE, L"KeyBridgeMutex");
    if (!mx) {
        MessageBoxW(nullptr, L"Failed to create mutex.", L"KeyBridge", MB_ICONERROR);
        StopGdiPlus();
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"KeyBridge is already running.",
                    L"KeyBridge", MB_ICONINFORMATION);
        CloseHandle(mx);
        StopGdiPlus();
        return 0;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kMainWindowClass;
    wc.hCursor       = g_curArrow;
    wc.hIcon         = LoadIcon(g_inst, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hIconSm       = (HICON)LoadImage(g_inst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.style         = CS_DROPSHADOW;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.",
                    L"KeyBridge", MB_ICONERROR);
        CloseHandle(mx);
        StopGdiPlus();
        return 1;
    }

    const int W  = 740, H  = 590;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kMainWindowClass,
        L"KeyBridge",
        WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
        sx, sy, W, H,
        nullptr, nullptr, hi, nullptr);

    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Failed to create main window.",
                    L"KeyBridge", MB_ICONERROR);
        CloseHandle(mx);
        StopGdiPlus();
        return 1;
    }

    ApplyWindowEffects(g_hwnd);
    SetAppDpi(g_hwnd);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, nullptr, 0);
    if (!g_hook) {
        MessageBoxW(nullptr,
            L"Failed to install keyboard hook.\n\nPlease run as administrator.",
            L"KeyBridge - Fatal Error", MB_ICONERROR);
        g_fc.Destroy();
        CloseHandle(mx);
        StopGdiPlus();
        return 1;
    }

    LoadConfig();

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIcon(g_inst, MAKEINTRESOURCE(IDI_APP_ICON));
    SyncTrayTip();
    g_trayOk = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);

    if (!g_trayOk) {
        MessageBoxW(nullptr,
            L"Could not add tray icon.\nClosing the window will exit the program.",
            L"KeyBridge - Warning", MB_ICONWARNING);
    }

    if (!TryRegisterToggleHotkey(g_hkMod, g_hkVk)) {
        std::wstring warn =
            L"Failed to register hotkey \"" + GetHotkeyString() +
            L"\".\nIt may be in use by another application.\n\n"
            L"You can change it from the main window.";
        MessageBoxW(nullptr, warn.c_str(), L"KeyBridge - Warning", MB_ICONWARNING);
    }

    if (g_startMinimized)
        ShowWindow(g_hwnd, SW_HIDE);
    else
        ShowWindow(g_hwnd, nShow);

    UpdateWindow(g_hwnd);
    UpdateScroll(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_fc.Destroy();

    if (g_pickClassOk) {
        UnregisterClassW(kPickerWindowClass, g_inst);
        g_pickClassOk = false;
    }

    Logo::Shutdown();
    CloseHandle(mx);
    StopGdiPlus();
    return (int)msg.wParam;
}