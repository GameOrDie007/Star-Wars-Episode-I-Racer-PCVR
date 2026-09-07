// Restore a force-feedback wheel's centring spring while the game runs.
//
// Out of the game a G923 has firm tension: that is the driver's default autocentre spring. When a
// DirectInput application acquires the device, DirectInput switches autocentre OFF, because an
// application that takes a force-feedback device is expected to drive the effects itself. SWE1R was
// written against Immersion's IFORCE2 and does not drive a modern wheel, so it turns the spring off
// and never replaces it. The wheel goes slack, and with nothing to damp small movements the
// steering feels twitchy no matter how it is calibrated.
//
// The fix is to put DIPROP_AUTOCENTER back on after the game acquires the device. That needs the
// device pointer, which only the game sees -- so CreateDevice is intercepted to reach it, and
// Acquire is intercepted so the property is set at a point where it sticks (setting it before
// acquisition does not).
//
// Interception is by vtable slot rather than by wrapping the interfaces. Wrapping means
// reimplementing every method of IDirectInput and IDirectInputDevice and getting the lifetime
// right; a slot patch touches two pointers. The trade is that a vtable is shared by every device
// of the same class, so the Acquire hook also runs for the keyboard and mouse -- harmless, since
// setting autocentre on a device without force feedback simply returns an error, which is ignored.
//
// Everything here fails soft. If any step does not work the original function is still called and
// the game behaves exactly as it did before; the only consequence is a slack wheel.

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
extern "C" int vr_wheel_autocenter(void);

namespace {

// COM methods are __stdcall on x86. REFGUID is a const GUID& in C++, which is a pointer at the ABI
// level, so a const GUID* here matches what the callee expects.
typedef HRESULT(__stdcall *CreateDevice_t)(void *self, const GUID *rguid, void **ppDevice,
                                           void *punkOuter);
typedef HRESULT(__stdcall *Acquire_t)(void *self);
typedef HRESULT(__stdcall *SetProperty_t)(void *self, const GUID *rguidProp,
                                          const DIPROPHEADER *pdiph);
typedef HRESULT(__stdcall *GetDeviceInfo_t)(void *self, DIDEVICEINSTANCEA *pdidi);

// IDirectInputA vtable: 0 QueryInterface, 1 AddRef, 2 Release, 3 CreateDevice.
const int kSlotCreateDevice = 3;
// IDirectInputDeviceA vtable: ... 5 GetProperty, 6 SetProperty, 7 Acquire.
const int kSlotSetProperty = 6;
const int kSlotAcquire = 7;
const int kSlotGetDeviceInfo = 15;

// Set once a device identifying itself as a wheel has been created, together with the
// identity needed to open our own handle on the same hardware later.
bool g_wheelDevicePresent = false;
GUID g_wheelGuid;
bool g_haveWheelGuid = false;
void *g_pDI = NULL;

CreateDevice_t g_origCreateDevice = nullptr;
Acquire_t g_origAcquire = nullptr;

void logf_once(const char *fmt, ...) {
    if (!hook_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(hook_log, fmt, ap);
    va_end(ap);
    fflush(hook_log);
}

// Replace one vtable entry, remembering the original the first time. Returns false and changes
// nothing if the memory cannot be made writable.
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

HRESULT __stdcall Acquire_hook(void *dev) {
    const HRESULT hr = g_origAcquire ? g_origAcquire(dev) : E_FAIL;
    if (FAILED(hr) || !vr_wheel_enabled() || !vr_wheel_autocenter())
        return hr;

    // Autocentre only means anything on a force-feedback device; everything else returns an error
    // that is deliberately ignored, because this same vtable serves the keyboard and mouse.
    void **vtbl = *(void ***) dev;
    SetProperty_t setProperty = (SetProperty_t) vtbl[kSlotSetProperty];

    DIPROPDWORD prop;
    ZeroMemory(&prop, sizeof(prop));
    prop.diph.dwSize = sizeof(DIPROPDWORD);
    prop.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    prop.diph.dwObj = 0;
    prop.diph.dwHow = DIPH_DEVICE;
    prop.dwData = DIPROPAUTOCENTER_ON;

    const HRESULT pr = setProperty(dev, &DIPROP_AUTOCENTER, &prop.diph);

    // One line per device, not per acquire: a game re-acquires on every focus change.
    static void *logged[8] = {};
    static int nlogged = 0;
    bool seen = false;
    for (int i = 0; i < nlogged; i++)
        if (logged[i] == dev)
            seen = true;
    if (!seen && nlogged < 8) {
        logged[nlogged++] = dev;
        logf_once("[wheel] autocenter on device %p: %s (hr=0x%08lx)\n", dev,
                  SUCCEEDED(pr) ? "ENABLED" : "not a force-feedback device, ignored",
                  (unsigned long) pr);
    }
    return hr;
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
    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
        note_device_type(*ppDevice);
        patch_slot(*ppDevice, kSlotAcquire, (void *) &Acquire_hook, (void **) &g_origAcquire);
    }
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

// Each object is named explicitly. The previous format asked for "any object" forty-four
// times over with nothing to tell the entries apart, and DirectInput rejected it outright.
// An entry with no matching object on the device is left at zero rather than failing.
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
        g_objFmt[n].dwType =
            DIDFT_AXIS | (i >= 6 ? DIDFT_MAKEINSTANCE(i - 6) : (DWORD) DIDFT_ANYINSTANCE);
        g_objFmt[n].dwFlags = DIDOI_ASPECTPOSITION;
        n++;
    }
    for (int i = 0; i < 4; i++) {
        g_objFmt[n].pguid = &kGuidPOV;
        g_objFmt[n].dwOfs = (DWORD) (offsetof(WheelState, pov) + i * sizeof(DWORD));
        g_objFmt[n].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(i);
        g_objFmt[n].dwFlags = 0;
        n++;
    }
    for (int i = 0; i < 32; i++) {
        // Buttons by instance, not by GUID: that is what fixes the order, and it is how the
        // device's own numbering reaches us intact rather than through the game's 16-slot map.
        g_objFmt[n].pguid = NULL;
        g_objFmt[n].dwOfs = (DWORD) (offsetof(WheelState, button) + i);
        g_objFmt[n].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i);
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
    logf_once("[wheel] autocenter hook %s\n",
              installed ? "installed" : "FAILED to install - wheel will stay slack");
}
