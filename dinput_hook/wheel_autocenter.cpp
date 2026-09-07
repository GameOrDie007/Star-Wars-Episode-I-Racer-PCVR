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

// Set once a device identifying itself as a wheel has been created.
bool g_wheelDevicePresent = false;

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

}// namespace

// Called from the dinput proxy once the real DirectInput object exists.
extern "C" int vr_wheel_device_present(void) {
    return g_wheelDevicePresent ? 1 : 0;
}

extern "C" void wheel_autocenter_install(void *pDI) {
    static bool installed = false;
    if (installed || pDI == nullptr)
        return;
    installed = patch_slot(pDI, kSlotCreateDevice, (void *) &CreateDevice_hook,
                           (void **) &g_origCreateDevice);
    logf_once("[wheel] autocenter hook %s\n",
              installed ? "installed" : "FAILED to install - wheel will stay slack");
}
