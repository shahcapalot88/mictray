#include <initguid.h>          
#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <math.h>
#include <wchar.h>

// ---------------------------------------------------------------- constants
enum { WM_TRAY = WM_APP + 1, WM_APP_MUTE, WM_APP_DEVICES, WM_APP_KEY };
enum { KEY_PTT_DOWN = 1, KEY_PTT_UP, KEY_TOGGLE, KEY_CAPTURED };
enum {
    IDM_DEV_DEFAULT = 1000, IDM_DEV_BASE = 1001, MAX_DEV = 32,
    IDM_STARTUP = 1100, IDM_PTT_SET, IDM_TOG_SET, IDM_PTT_CLR, IDM_TOG_CLR, IDM_EXIT
};

static const WCHAR* CFG_KEY = L"Software\\MicTray";
static const WCHAR* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* RUN_VAL = L"MicTray";

struct Hotkey { DWORD vk; DWORD mods; };
struct Dev    { WCHAR id[160]; WCHAR name[128]; };

// ---------------------------------------------------------------- audio callbacks
struct VolCb : IAudioEndpointVolumeCallback {
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == __uuidof(IUnknown) || r == __uuidof(IAudioEndpointVolumeCallback)) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return 2; }   // static object, never freed
    STDMETHODIMP_(ULONG) Release() { return 1; }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA);
};

struct NotifCb : IMMNotificationClient {
    STDMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMMNotificationClient)) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return 2; }
    STDMETHODIMP_(ULONG) Release() { return 1; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD)        { Poke(); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR)                      { Poke(); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR)                    { Poke(); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow f, ERole, LPCWSTR) { if (f == eCapture) Poke(); return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) { return S_OK; }
    void Poke();
};

// ---------------------------------------------------------------- state
static struct {
    HWND hwnd;
    IMMDeviceEnumerator*  enumr;
    IAudioEndpointVolume* vol;
    WCHAR devId[160];            // empty = follow system default
    WCHAR curName[128];
    HICON iconMuted, iconLive;
    bool  muted;
    Hotkey ptt, tog;
    bool  pttHeld, togHeld;
    int   capture;               // 0 none, 1 = capturing PTT key, 2 = capturing toggle key
    HHOOK hook;
    UINT  taskbarMsg;
} g;

// Hands unused pages back to Windows (lowers the resident/working-set size shown in Task Manager).
static void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

static VolCb   g_volCb;
static NotifCb g_notifCb;
static Dev     g_devs[MAX_DEV];
static int     g_ndevs;

STDMETHODIMP VolCb::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) {
    PostMessageW(g.hwnd, WM_APP_MUTE, 0, 0);       // called on an audio thread
    return S_OK;
}
void NotifCb::Poke() { PostMessageW(g.hwnd, WM_APP_DEVICES, 0, 0); }

// ---------------------------------------------------------------- settings
static DWORD GetDw(const WCHAR* name, DWORD def) {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, CFG_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? v : def;
}
static void SetDw(const WCHAR* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, CFG_KEY, name, REG_DWORD, &v, sizeof(v));
}
static void LoadSettings() {
    DWORD cb = sizeof(g.devId);
    if (RegGetValueW(HKEY_CURRENT_USER, CFG_KEY, L"DeviceId", RRF_RT_REG_SZ, nullptr, g.devId, &cb) != ERROR_SUCCESS)
        g.devId[0] = 0;
    g.ptt.vk = GetDw(L"PttVk", 0);    g.ptt.mods = GetDw(L"PttMods", 0);
    g.tog.vk = GetDw(L"ToggleVk", 0); g.tog.mods = GetDw(L"ToggleMods", 0);
}
static void SaveSettings() {
    RegSetKeyValueW(HKEY_CURRENT_USER, CFG_KEY, L"DeviceId", REG_SZ, g.devId,
                    (DWORD)((wcslen(g.devId) + 1) * sizeof(WCHAR)));
    SetDw(L"PttVk", g.ptt.vk);    SetDw(L"PttMods", g.ptt.mods);
    SetDw(L"ToggleVk", g.tog.vk); SetDw(L"ToggleMods", g.tog.mods);
}

static bool IsAutostart() {
    WCHAR buf[MAX_PATH + 4]; DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VAL, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS;
}
static void SetAutostart(bool on) {
    if (on) {
        WCHAR path[MAX_PATH], val[MAX_PATH + 4];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        swprintf_s(val, L"\"%s\"", path);
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VAL, REG_SZ, val, (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VAL);
    }
}

// ---------------------------------------------------------------- icons (rasterised at runtime, anti-aliased)
static float DistSeg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float cx = ax + t * dx - px, cy = ay + t * dy - py;
    return sqrtf(cx * cx + cy * cy);
}
// Shapes in a 32x32 design space.
static bool InShape(float x, float y, bool slash) {
    if (slash) {
        float ds = DistSeg(x, y, 5, 5, 27, 27);
        if (ds <= 1.6f) return true;      // the slash itself
        if (ds <= 3.4f) return false;     // gap carved around it
    }
    if (DistSeg(x, y, 16, 9, 16, 14) <= 5.0f) return true;                 // capsule
    float dx = x - 16, dy = y - 14, d = sqrtf(dx * dx + dy * dy);
    if (y >= 14 && d >= 7.0f && d <= 9.2f) return true;                    // cradle
    if (x >= 15 && x <= 17 && y >= 22 && y <= 26) return true;             // stem
    if (x >= 11 && x <= 21 && y >= 26 && y <= 28.2f) return true;          // base
    return false;
}
static HICON MakeIcon(int sz, BYTE r, BYTE gc, BYTE b, bool slash) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    DWORD* px = (DWORD*)bits;
    const float k = 32.0f / sz;
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++) {
            int cov = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    if (InShape((x + (sx + 0.5f) / 4) * k, (y + (sy + 0.5f) / 4) * k, slash)) cov++;
            DWORD a = (DWORD)(cov * 255 / 16);
            px[y * sz + x] = (a << 24) | ((DWORD)r << 16) | ((DWORD)gc << 8) | b;   // straight alpha
        }
    BYTE zeros[((128 + 15) / 16) * 2 * 128] = {};
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, zeros);
    ICONINFO ii = {}; ii.fIcon = TRUE; ii.hbmMask = mask; ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(color);
    return ic;
}

// ---------------------------------------------------------------- tray
static NOTIFYICONDATAW Nid(UINT flags) {
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof(n); n.hWnd = g.hwnd; n.uID = 1; n.uFlags = flags;
    return n;
}
static void FillIconAndTip(NOTIFYICONDATAW& n) {
    n.hIcon = g.muted ? g.iconMuted : g.iconLive;
    if (g.vol) swprintf_s(n.szTip, L"Mic %s - %s", g.muted ? L"MUTED" : L"live", g.curName);
    else       wcscpy_s(n.szTip, L"No input device");
}
static HICON g_shownIcon;                // what the tray currently displays
static WCHAR g_shownTip[128];
static void TrayAdd() {
    NOTIFYICONDATAW n = Nid(NIF_ICON | NIF_TIP | NIF_MESSAGE);
    n.uCallbackMessage = WM_TRAY;
    FillIconAndTip(n);
    if (Shell_NotifyIconW(NIM_ADD, &n)) { g_shownIcon = n.hIcon; wcscpy_s(g_shownTip, n.szTip); }
}
static void TrayUpdate() {
    NOTIFYICONDATAW n = Nid(NIF_ICON | NIF_TIP);
    FillIconAndTip(n);
    // The volume callback fires on every level change, not only on mute changes: skip no-op updates.
    if (n.hIcon == g_shownIcon && wcscmp(n.szTip, g_shownTip) == 0) return;
    if (Shell_NotifyIconW(NIM_MODIFY, &n)) { g_shownIcon = n.hIcon; wcscpy_s(g_shownTip, n.szTip); }
}
static void Balloon(const WCHAR* title, const WCHAR* text) {
    NOTIFYICONDATAW n = Nid(NIF_INFO);
    wcscpy_s(n.szInfoTitle, title); wcscpy_s(n.szInfo, text);
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------- endpoint handling
static void DevName(IMMDevice* d, WCHAR* out, size_t cch) {
    out[0] = 0;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            wcsncpy_s(out, cch, pv.pwszVal, _TRUNCATE);
        PropVariantClear(&pv);
        ps->Release();
    }
}
static void ReleaseEndpoint() {
    if (g.vol) {
        g.vol->UnregisterControlChangeNotify(&g_volCb);
        g.vol->Release();
        g.vol = nullptr;
    }
}
// Uses the saved device if it is active, otherwise falls back to the system default capture device
// (the saved ID is kept, so the preferred mic is picked up again when it returns).
static bool AcquireEndpoint() {
    ReleaseEndpoint();
    g.curName[0] = 0;
    IMMDevice* dev = nullptr;
    if (g.devId[0] && SUCCEEDED(g.enumr->GetDevice(g.devId, &dev))) {
        DWORD st = 0;
        if (FAILED(dev->GetState(&st)) || st != DEVICE_STATE_ACTIVE) { dev->Release(); dev = nullptr; }
    }
    if (!dev && FAILED(g.enumr->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) return false;
    bool ok = SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&g.vol));
    if (ok) {
        DevName(dev, g.curName, _countof(g.curName));
        g.vol->RegisterControlChangeNotify(&g_volCb);
    }
    dev->Release();
    return ok;
}
static void RefreshState() {
    BOOL m = FALSE;
    if (g.vol && SUCCEEDED(g.vol->GetMute(&m))) g.muted = (m != FALSE);
    else if (!g.vol) g.muted = true;
    TrayUpdate();
}
static void SetMuted(bool mute) {
    if (!g.vol && !AcquireEndpoint()) { RefreshState(); return; }
    if (FAILED(g.vol->SetMute(mute, nullptr))) {          // stale endpoint? re-acquire once
        if (AcquireEndpoint()) g.vol->SetMute(mute, nullptr);
    }
    RefreshState();
}
static void ToggleMute() {
    if (!g.vol && !AcquireEndpoint()) { RefreshState(); return; }
    BOOL m = FALSE;
    g.vol->GetMute(&m);
    SetMuted(!m);
}
static int EnumDevices() {
    g_ndevs = 0;
    IMMDeviceCollection* c = nullptr;
    if (FAILED(g.enumr->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &c))) return 0;
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n && g_ndevs < MAX_DEV; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(c->Item(i, &d))) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) {
            wcsncpy_s(g_devs[g_ndevs].id, id, _TRUNCATE);
            DevName(d, g_devs[g_ndevs].name, _countof(g_devs[g_ndevs].name));
            g_ndevs++;
            CoTaskMemFree(id);
        }
        d->Release();
    }
    c->Release();
    return g_ndevs;
}

// ---------------------------------------------------------------- hotkeys (low-level keyboard hook => key-down AND key-up)
static bool IsModifierVk(DWORD vk) {
    return (vk >= 0x10 && vk <= 0x12) || (vk >= 0xA0 && vk <= 0xA5) || vk == VK_LWIN || vk == VK_RWIN;
}
static DWORD CurMods() {
    DWORD m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
    return m;
}
static LRESULT CALLBACK KbProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        DWORD vk = k->vkCode;
        bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
        bool up   = (w == WM_KEYUP   || w == WM_SYSKEYUP);
        if (g.capture) {
            if (down && !IsModifierVk(vk)) {
                bool esc = (vk == VK_ESCAPE);
                DWORD mods = esc ? 0 : CurMods();
                int kind = g.capture; g.capture = 0;
                PostMessageW(g.hwnd, WM_APP_KEY, KEY_CAPTURED, (LPARAM)((esc ? 0 : vk) | (mods << 8) | (kind << 16)));
                return 1;   // swallow the captured key
            }
        } else if (down) {
            if (g.ptt.vk && vk == g.ptt.vk && !g.pttHeld && CurMods() == g.ptt.mods) {
                g.pttHeld = true;  PostMessageW(g.hwnd, WM_APP_KEY, KEY_PTT_DOWN, 0);
            }
            if (g.tog.vk && vk == g.tog.vk && !g.togHeld && CurMods() == g.tog.mods) {
                g.togHeld = true;  PostMessageW(g.hwnd, WM_APP_KEY, KEY_TOGGLE, 0);
            }
        } else if (up) {
            if (g.pttHeld && vk == g.ptt.vk) { g.pttHeld = false; PostMessageW(g.hwnd, WM_APP_KEY, KEY_PTT_UP, 0); }
            if (vk == g.tog.vk) g.togHeld = false;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}
// The low-level hook exists only while a hotkey is configured or a key is being captured; with no
// hotkeys, Windows doesn't need to route every system-wide keystroke through this process.
static void UpdateHook() {
    bool want = g.capture || g.ptt.vk || g.tog.vk;
    if (want && !g.hook)       g.hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    else if (!want && g.hook)  { UnhookWindowsHookEx(g.hook); g.hook = nullptr; }
}
static bool IsExtendedVk(DWORD vk) {
    switch (vk) {
    case VK_RCONTROL: case VK_RMENU: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_NUMLOCK: case VK_DIVIDE: return true;
    }
    return false;
}
static void KeyText(const Hotkey& h, WCHAR* out, size_t cch) {
    out[0] = 0;
    if (!h.vk) { wcscpy_s(out, cch, L"(none)"); return; }
    if (h.mods & MOD_CONTROL) wcscat_s(out, cch, L"Ctrl+");
    if (h.mods & MOD_ALT)     wcscat_s(out, cch, L"Alt+");
    if (h.mods & MOD_SHIFT)   wcscat_s(out, cch, L"Shift+");
    if (h.mods & MOD_WIN)     wcscat_s(out, cch, L"Win+");
    LONG lp = (LONG)(MapVirtualKeyW(h.vk, MAPVK_VK_TO_VSC) << 16);
    if (IsExtendedVk(h.vk)) lp |= (1 << 24);
    WCHAR kn[64];
    if (GetKeyNameTextW(lp, kn, 64) > 0) wcscat_s(out, cch, kn);
    else { WCHAR t[16]; swprintf_s(t, L"VK 0x%02X", h.vk); wcscat_s(out, cch, t); }
}

// ---------------------------------------------------------------- menu
static void AddItem(HMENU m, UINT flags, UINT id, const WCHAR* text) {
    WCHAR b[300]; int j = 0;                       // escape '&' so it isn't treated as an accelerator
    for (int i = 0; text[i] && j < 290; i++) { b[j++] = text[i]; if (text[i] == L'&') b[j++] = L'&'; }
    b[j] = 0;
    AppendMenuW(m, MF_STRING | flags, id, b);
}
static void ShowMenu() {
    EnumDevices();
    HMENU menu = CreatePopupMenu(), sub = CreatePopupMenu(), hk = CreatePopupMenu();

    AddItem(sub, g.devId[0] ? 0 : MF_CHECKED, IDM_DEV_DEFAULT, L"System default");
    for (int i = 0; i < g_ndevs; i++)
        AddItem(sub, wcscmp(g_devs[i].id, g.devId) == 0 ? MF_CHECKED : 0, IDM_DEV_BASE + i, g_devs[i].name);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Select Input");

    AppendMenuW(menu, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");

    WCHAR kt[96], lbl[200];
    KeyText(g.ptt, kt, _countof(kt)); swprintf_s(lbl, L"Push to Talk: %s  (click to change)", kt);
    AddItem(hk, 0, IDM_PTT_SET, lbl);
    KeyText(g.tog, kt, _countof(kt)); swprintf_s(lbl, L"Toggle: %s  (click to change)", kt);
    AddItem(hk, 0, IDM_TOG_SET, lbl);
    AppendMenuW(hk, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hk, MF_STRING | (g.ptt.vk ? 0 : MF_GRAYED), IDM_PTT_CLR, L"Clear Push to Talk");
    AppendMenuW(hk, MF_STRING | (g.tog.vk ? 0 : MF_GRAYED), IDM_TOG_CLR, L"Clear Toggle");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)hk, L"Hotkeys");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g.hwnd);                   // required so the menu dismisses on outside click
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);                             // also destroys the submenus
    TrimMemory();
}

// ---------------------------------------------------------------- window procedure
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TRAY:
        if (l == WM_LBUTTONUP) ToggleMute();
        else if (l == WM_RBUTTONUP) ShowMenu();
        return 0;

    case WM_APP_MUTE:                 // mute changed elsewhere (Windows UI, another app)
        RefreshState();
        return 0;

    case WM_APP_DEVICES:              // device added/removed/default changed: debounce, then re-acquire
        SetTimer(h, 1, 300, nullptr);
        return 0;
    case WM_TIMER:
        if (w == 1) { KillTimer(h, 1); AcquireEndpoint(); RefreshState(); }
        return 0;

    case WM_APP_KEY:
        switch (w) {
        case KEY_PTT_DOWN: SetMuted(false); break;
        case KEY_PTT_UP:   SetMuted(true);  break;
        case KEY_TOGGLE:   ToggleMute();    break;
        case KEY_CAPTURED: {
            DWORD vk = (DWORD)(l & 0xFF), mods = (DWORD)((l >> 8) & 0xFF), kind = (DWORD)((l >> 16) & 0xFF);
            if (!vk) { Balloon(L"Mic Tray", L"Hotkey change cancelled."); UpdateHook(); break; }
            Hotkey& hkp = (kind == 1) ? g.ptt : g.tog;
            hkp.vk = vk; hkp.mods = mods;
            SaveSettings();
            WCHAR kt[96], msg[160];
            KeyText(hkp, kt, _countof(kt));
            swprintf_s(msg, L"%s set to %s", kind == 1 ? L"Push to Talk" : L"Toggle", kt);
            Balloon(L"Mic Tray", msg);
            if (kind == 1) SetMuted(true);          // push-to-talk mode: mic is muted unless key is held
            UpdateHook();
            break;
        }
        }
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(w);
        if (id == IDM_DEV_DEFAULT) {
            g.devId[0] = 0; SaveSettings(); AcquireEndpoint(); RefreshState();
        } else if (id >= IDM_DEV_BASE && id < IDM_DEV_BASE + (UINT)g_ndevs) {
            wcscpy_s(g.devId, g_devs[id - IDM_DEV_BASE].id); SaveSettings(); AcquireEndpoint(); RefreshState();
        } else switch (id) {
        case IDM_STARTUP: SetAutostart(!IsAutostart()); break;
        case IDM_PTT_SET: g.capture = 1; UpdateHook(); Balloon(L"Push to Talk", L"Press the key (or combo) to use. Esc cancels."); break;
        case IDM_TOG_SET: g.capture = 2; UpdateHook(); Balloon(L"Toggle", L"Press the key (or combo) to use. Esc cancels."); break;
        case IDM_PTT_CLR: g.ptt = Hotkey{0, 0}; g.pttHeld = false; SaveSettings(); UpdateHook(); break;
        case IDM_TOG_CLR: g.tog = Hotkey{0, 0}; g.togHeld = false; SaveSettings(); UpdateHook(); break;
        case IDM_EXIT:    DestroyWindow(h); break;
        }
        return 0;
    }

    case WM_DESTROY: {
        NOTIFYICONDATAW n = Nid(0);
        Shell_NotifyIconW(NIM_DELETE, &n);
        if (g.hook) UnhookWindowsHookEx(g.hook);
        if (g.enumr) g.enumr->UnregisterEndpointNotificationCallback(&g_notifCb);
        ReleaseEndpoint();
        if (g.enumr) g.enumr->Release();
        if (g.iconMuted) DestroyIcon(g.iconMuted);
        if (g.iconLive)  DestroyIcon(g.iconLive);
        PostQuitMessage(0);
        return 0;
    }
    }
    if (m == g.taskbarMsg) { TrayAdd(); return 0; }   // Explorer restarted
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- entry point
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\MicTray_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&g.enumr))) return 1;
    LoadSettings();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"MicTrayWnd";
    RegisterClassW(&wc);
    // Hidden top-level window (not message-only) so it receives the "TaskbarCreated" broadcast.
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MicTray", WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, hi, nullptr);
    g.taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");

    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz < 16)  sz = 16;
    if (sz > 128) sz = 128;
    g.iconLive  = MakeIcon(sz,  46, 204, 113, false);   // green
    g.iconMuted = MakeIcon(sz, 231,  76,  60, true);    // red + slash

    AcquireEndpoint();
    g.enumr->RegisterEndpointNotificationCallback(&g_notifCb);
    RefreshState();
    TrayAdd();
    if (g.ptt.vk) SetMuted(true);                       // push-to-talk configured: start muted

    UpdateHook();
    TrimMemory();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
