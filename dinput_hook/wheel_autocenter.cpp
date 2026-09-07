// Identify the game's DirectInput devices, and read the wheel directly.
//
// (The file is still named for a centring-spring feature that used to live here. It was
// removed in v1.2.1 as dead code: see below.)
//
// CreateDevice is intercepted so the mod can see every device the game opens: what type each one
// is (a wheel says so, and no amount of watching axis values ever could), and the instance GUID
// needed to open a second handle on the same wheel further down this file.
//
// Interception is by vtable slot rather than by wrapping the interface. Wrapping means
// reimplementing every method of IDirectInput and getting the lifetime right; a slot patch touches
// two pointers. There is exactly one IDirectInput object, created once, so there is one vtable and
// one original to remember.
//
// v1.2 also patched Acquire in each DEVICE's vtable, to put DIPROP_AUTOCENTER back after the game
// acquired a wheel. That is gone as of v1.2.1, because it had never once succeeded: every device
// in every session logged answered 'not a force-feedback device' (0x800700aa), the G923 included.
// The centring spring is available in the wheel's own driver software, which is what the README
// has always recommended.
//
// It was briefly suspected of causing a race-end crash, on the theory that keyboard, mouse and
// joystick are separate classes with separate vtables while only one original was stored. That
// was measured and is false: a probe against the real dinput.dll with a G923 attached returns
// vtable 6c2ae050 for all three, so one original was correct and the second patch_slot call took
// its already-patched early return. The actual crash was a use-after-free in a debug print, in
// swrObjJdge_delta.cpp. Recorded here because the wrong answer is the more plausible-sounding one
// and someone will reach for it again.
//
// If autocentre is ever attempted again it needs evidence that SetProperty can succeed at all on
// this game's interface version -- and, if it is ever applied to devices of more than one class,
// a table of originals keyed by vtable.
//
// Everything here fails soft. If any step does not work the original function is still called and
// the game behaves exactly as it did before.

#define DIRECTINPUT_VERSION 0x0700

#include <windows.h>
#include <dinput.h>
#include <stdarg.h>
#include <stddef.h>// offsetof, for the custom data format
#include <stdio.h>

extern "C" {
extern FILE *hook_log;
}

extern "C" int vr_wheel_enabled(void);

namespace {

// COM methods are __stdcall on x86. REFGUID is a const GUID& in C++, which is a pointer at the ABI
// level, so a const GUID* here matches what the callee expects.
typedef HRESULT(__stdcall *CreateDevice_t)(void *self, const GUID *rguid, void **ppDevice,
                                           void *punkOuter);
typedef HRESULT(__stdcall *Acquire_t)(void *self);
typedef HRESULT(__stdcall *GetDeviceInfo_t)(void *self, DIDEVICEINSTANCEA *pdidi);

// IDirectInputA vtable: 0 QueryInterface, 1 AddRef, 2 Release, 3 CreateDevice.
const int kSlotCreateDevice = 3;
// IDirectInputDeviceA vtable: ... 6 SetProperty, 7 Acquire.
const int kSlotAcquire = 7;
const int kSlotGetDeviceInfo = 15;

// Set once a device identifying itself as a wheel has been created, together with the
// identity needed to open our own handle on the same hardware later.
bool g_wheelDevicePresent = false;
GUID g_wheelGuid;
bool g_haveWheelGuid = false;
void *g_pDI = NULL;

CreateDevice_t g_origCreateDevice = nullptr;

void logf_once(const char *fmt, ...) {
    if (!hook_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(hook_log, fmt, ap);
    va_end(ap);
    fflush(hook_log);
}

// Replace one vtable entry, remembering the original. Returns false and changes nothing if the
// memory cannot be made writable.
//
// One original, so ONE vtable. If this is ever called for objects whose classes have distinct
// vtables, it stores the first class's function and then calls it for all of them. This
// dinput.dll happens to give keyboard, mouse and joystick a single shared vtable, so that has
// never bitten -- do not rely on it. It is used only on the single IDirectInput object.
bool patch_slot(void *obj, int index, void *hook, void **out_orig) {
    if (obj == nullptr)
        return false;
    void **vtbl = *(void ***) obj;
    if (vtbl == nullptr)
        return false;
    if (vtbl[index] == hook)
        return true;// this vtable is shared and was patched already
    if (*out_orig == nullptr)
        *out_orig = vtbl[index];

    DWORD prot = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_READWRITE, &prot))
        return false;
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), prot, &prot);
    return true;
}

// A wheel identifies itself. Axis values never could: a pad's left stick is axis 0, the same
// axis a wheel's rim reports on, which is why watching for movement was hopeless.
void note_device_type(void *dev) {
    void **vtbl = *(void ***) dev;
    GetDeviceInfo_t getInfo = (GetDeviceInfo_t) vtbl[kSlotGetDeviceInfo];

    DIDEVICEINSTANCEA info;
    ZeroMemory(&info, sizeof(info));
    info.dwSize = sizeof(DIDEVICEINSTANCEA);
    HRESULT hr = getInfo(dev, &info);
    if (FAILED(hr)) {
        // The game may have created the interface at an older DirectInput version, which
        // validates dwSize against its own smaller structure.
        ZeroMemory(&info, sizeof(info));
        info.dwSize = sizeof(DIDEVICEINSTANCE_DX3A);
        hr = getInfo(dev, &info);
    }
    if (FAILED(hr))
        return;

    const BYTE type = (BYTE) (info.dwDevType & 0xFF);
    const BYTE subtype = (BYTE) ((info.dwDevType >> 8) & 0xFF);
    // 0x14 is DI8DEVTYPE_DRIVING; on older interfaces a wheel is a joystick with subtype
    // WHEEL. Accept either, since which appears depends on the version the game asked for.
    const bool isWheel =
        (type == 0x14) || (type == DIDEVTYPE_JOYSTICK && subtype == DIDEVTYPEJOYSTICK_WHEEL);
    if (isWheel && !g_wheelDevicePresent) {
        g_wheelDevicePresent = true;
        g_wheelGuid = info.guidInstance;
        g_haveWheelGuid = true;
        logf_once("[wheel] driving device detected: '%s' (devType=0x%08lx)\n",
                  info.tszProductName, (unsigned long) info.dwDevType);
    }
}

HRESULT __stdcall CreateDevice_hook(void *self, const GUID *rguid, void **ppDevice,
                                    void *punkOuter) {
    const HRESULT hr =
        g_origCreateDevice ? g_origCreateDevice(self, rguid, ppDevice, punkOuter) : E_FAIL;
    // Identify the device, and nothing else. v1.2 also patched each device's Acquire slot here;
    // see the note at the top of this file for why that is gone.
    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr)
        note_device_type(*ppDevice);
    return hr;
}

// ---------------------------------------------------------------------------
// Direct device read
//
// The game maps only 16 buttons plus a hat, and three axes. This wheel reports 23
// buttons and four axes, so Start, Back and the clutch never reach any array the
// mod can read. Opening our own device on the same hardware sidesteps the game's
// input layer entirely.
//
// Non-exclusive and background, so it coexists with the game's own acquisition
// rather than competing for the device.
// ---------------------------------------------------------------------------

struct WheelState {
    LONG axis[8];
    DWORD pov[4];
    BYTE button[32];
};

// Object GUIDs, defined locally so no import is added. Values copied from dinput.h.
const GUID kGuidXAxis  = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidYAxis  = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidZAxis  = {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidRxAxis = {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidRyAxis = {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidRzAxis = {0xA36D02E3, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidSlider = {0xA36D02E4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kGuidPOV    = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};

DIOBJECTDATAFORMAT g_objFmt[8 + 4 + 32];
DIDATAFORMAT g_fmt;
bool g_fmtReady = false;

// Each object is named explicitly, and every one is marked DIDFT_OPTIONAL.
//
// OPTIONAL is the part that matters: without it EVERY object in the format must exist on
// the device, and this format asks for 8 axes, 4 POVs and 32 buttons while the wheel has 4,
// 1 and 23. The unmatched entries failed the whole call with E_INVALIDARG. The stock
// joystick formats mark every entry optional for exactly this reason -- one format has to
// serve devices of every shape.
void build_format() {
    if (g_fmtReady)
        return;
    const GUID *axisGuid[8] = {&kGuidXAxis,  &kGuidYAxis,  &kGuidZAxis,  &kGuidRxAxis,
                               &kGuidRyAxis, &kGuidRzAxis, &kGuidSlider, &kGuidSlider};
    int n = 0;
    for (int i = 0; i < 8; i++) {
        g_objFmt[n].pguid = axisGuid[i];
        g_objFmt[n].dwOfs = (DWORD) (i * sizeof(LONG));
        // The two sliders are the only pair sharing a GUID, so they are the only entries
        // needing an instance number to tell them apart.
        g_objFmt[n].dwType = DIDFT_AXIS | DIDFT_OPTIONAL |
                             (i >= 6 ? DIDFT_MAKEINSTANCE(i - 6) : (DWORD) DIDFT_ANYINSTANCE);
        g_objFmt[n].dwFlags = DIDOI_ASPECTPOSITION;
        n++;
    }
    for (int i = 0; i < 4; i++) {
        g_objFmt[n].pguid = &kGuidPOV;
        g_objFmt[n].dwOfs = (DWORD) (offsetof(WheelState, pov) + i * sizeof(DWORD));
        g_objFmt[n].dwType = DIDFT_POV | DIDFT_OPTIONAL | DIDFT_MAKEINSTANCE(i);
        g_objFmt[n].dwFlags = 0;
        n++;
    }
    for (int i = 0; i < 32; i++) {
        // Buttons by instance, not by GUID: that is what fixes the order, and it is how the
        // device's own numbering reaches us intact rather than through the game's 16-slot map.
        g_objFmt[n].pguid = NULL;
        g_objFmt[n].dwOfs = (DWORD) (offsetof(WheelState, button) + i);
        g_objFmt[n].dwType = DIDFT_BUTTON | DIDFT_OPTIONAL | DIDFT_MAKEINSTANCE(i);
        g_objFmt[n].dwFlags = 0;
        n++;
    }
    g_fmt.dwSize = sizeof(DIDATAFORMAT);
    g_fmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    g_fmt.dwFlags = DIDF_ABSAXIS;
    g_fmt.dwDataSize = sizeof(WheelState);
    g_fmt.dwNumObjs = (DWORD) n;
    g_fmt.rgodf = g_objFmt;
    g_fmtReady = true;
}

typedef HRESULT(__stdcall *QueryInterface_t)(void *, const GUID *, void **);
typedef HRESULT(__stdcall *SetDataFormat_t)(void *, const DIDATAFORMAT *);
typedef HRESULT(__stdcall *SetCoopLevel_t)(void *, HWND, DWORD);
typedef HRESULT(__stdcall *GetDeviceState_t)(void *, DWORD, void *);
typedef HRESULT(__stdcall *Poll_t)(void *);

// Counted from the header rather than remembered: QueryInterface 0, GetDeviceState 9,
// SetDataFormat 11, SetCooperativeLevel 13, and Poll 25 on IDirectInputDevice2.
const int kSlotQueryInterface = 0;
const int kSlotGetDeviceState = 9;
const int kSlotSetDataFormat = 11;
const int kSlotSetCoopLevel = 13;
const int kSlotPoll = 25;

void *g_directDev = NULL;// IDirectInputDevice2A once QueryInterface succeeds
bool g_directTried = false;
bool g_directOk = false;
WheelState g_state;

void direct_try_open() {
    if (g_directTried || !g_haveWheelGuid || g_pDI == NULL || g_origCreateDevice == NULL)
        return;

    // A window is needed for SetCooperativeLevel, and there is none during
    // DirectInputCreate, so opening is deferred until one exists.
    HWND hwnd = GetForegroundWindow();
    if (hwnd == NULL)
        return;

    g_directTried = true;
    build_format();

    void *dev = NULL;
    HRESULT hr = g_origCreateDevice(g_pDI, &g_wheelGuid, &dev, NULL);
    if (FAILED(hr) || dev == NULL) {
        logf_once("[wheel] direct: CreateDevice failed (0x%08lx)\n", (unsigned long) hr);
        return;
    }

    // The pollable interface. Without it the Poll slot cannot be called safely, since a
    // base IDirectInputDevice vtable is shorter and slot 25 would be somebody else's code.
    void **vtbl = *(void ***) dev;
    QueryInterface_t qi = (QueryInterface_t) vtbl[kSlotQueryInterface];
    static const GUID kIID_IDirectInputDevice2A = {
        0x5944E682, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    void *dev2 = NULL;
    if (FAILED(qi(dev, &kIID_IDirectInputDevice2A, &dev2)) || dev2 == NULL) {
        logf_once("[wheel] direct: no IDirectInputDevice2 - cannot poll\n");
        return;
    }

    void **v2 = *(void ***) dev2;
    SetDataFormat_t setFmt = (SetDataFormat_t) v2[kSlotSetDataFormat];
    SetCoopLevel_t setCoop = (SetCoopLevel_t) v2[kSlotSetCoopLevel];
    Acquire_t acquire = (Acquire_t) v2[kSlotAcquire];

    hr = setFmt(dev2, &g_fmt);
    if (FAILED(hr)) {
        logf_once("[wheel] direct: SetDataFormat failed (0x%08lx)\n", (unsigned long) hr);
        return;
    }
    // Background and non-exclusive: the game owns the device, this only observes it.
    hr = setCoop(dev2, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(hr)) {
        logf_once("[wheel] direct: SetCooperativeLevel failed (0x%08lx)\n", (unsigned long) hr);
        return;
    }
    hr = acquire(dev2);
    if (FAILED(hr)) {
        logf_once("[wheel] direct: Acquire failed (0x%08lx) - will retry\n", (unsigned long) hr);
        g_directTried = false;// transient; the game may not have finished starting up
        return;
    }

    g_directDev = dev2;
    g_directOk = true;
    logf_once("[wheel] direct read ACTIVE - all buttons and axes now readable\n");
}

}// namespace

extern "C" void vr_wheel_direct_poll(void) {
    if (!g_directOk) {
        direct_try_open();
        return;
    }
    void **v2 = *(void ***) g_directDev;
    Poll_t poll = (Poll_t) v2[kSlotPoll];
    GetDeviceState_t getState = (GetDeviceState_t) v2[kSlotGetDeviceState];

    poll(g_directDev);
    HRESULT hr = getState(g_directDev, sizeof(WheelState), &g_state);
    if (FAILED(hr)) {
        // Lost to a focus change; re-acquire and try again next frame.
        Acquire_t acquire = (Acquire_t) v2[kSlotAcquire];
        acquire(g_directDev);
    }
}

extern "C" int vr_wheel_direct_ok(void) {
    return g_directOk ? 1 : 0;
}

// Button 0..31, in the device's own order rather than the game's truncated map.
extern "C" int vr_wheel_direct_button(int i) {
    if (!g_directOk || i < 0 || i >= 32)
        return 0;
    return (g_state.button[i] & 0x80) ? 1 : 0;
}

// Axis 0..7, raw. The clutch is here even though the game only reads three axes.
extern "C" int vr_wheel_direct_axis(int i) {
    if (!g_directOk || i < 0 || i >= 8)
        return 0;
    return (int) g_state.axis[i];
}

// POV 0..3 in hundredths of a degree, or -1 when centred.
extern "C" int vr_wheel_direct_pov(int i) {
    if (!g_directOk || i < 0 || i >= 4)
        return -1;
    const DWORD v = g_state.pov[i];
    return (LOWORD(v) == 0xFFFF) ? -1 : (int) v;
}

namespace {
}// namespace

// Called from the dinput proxy once the real DirectInput object exists.
extern "C" int vr_wheel_device_present(void) {
    return g_wheelDevicePresent ? 1 : 0;
}

extern "C" void wheel_autocenter_install(void *pDI) {
    static bool installed = false;
    if (installed || pDI == nullptr)
        return;
    g_pDI = pDI;
    installed = patch_slot(pDI, kSlotCreateDevice, (void *) &CreateDevice_hook,
                           (void **) &g_origCreateDevice);
    logf_once("[wheel] device hook %s\n",
              installed ? "installed" : "FAILED to install - no wheel support");
}
