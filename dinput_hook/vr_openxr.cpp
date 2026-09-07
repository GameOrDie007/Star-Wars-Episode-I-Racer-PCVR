// OpenXR backend for the SWE1R PCVR port. Implements the same vr_probe.h interface the OpenVR
// backend did, so the renderer is untouched: per-eye frusta, the view composition, the depth-clear
// fix, the HUD layer and the culling widening all carry over unchanged.
//
// Why OpenXR is possible here at all: the game and this DLL are 32-bit, and SteamVR ships its
// OpenXR loader as win64 only -- which is what made me write the OpenVR backend first. But
// VirtualDesktop's VDXR registers a genuine i686 runtime under
// HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1, so a 32-bit OpenXR app works as long as VDXR (or
// another 32-bit-capable runtime) is active. SteamVR alone still cannot host us.
//
// Two structural differences from the OpenVR backend, both of which shape the code below:
//
//  1. The session is bound to the GL context (HDC + HGLRC), so it CANNOT be created during DLL
//     attach the way VR_InitInternal was. Init is deferred to the first frame, once GLFW's context
//     is current. That also removes the class of hang that repeatedly wedged startup: nothing
//     blocking runs before the game has a window.
//
//  2. You render into the runtime's swapchain images rather than handing it your own texture. So
//     vr_submit_eye blits our finished eye texture into an acquired swapchain image, and the frame
//     is closed by xrEndFrame with a projection layer covering both views.

#include "vr_probe.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>

#include <glad/glad.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "hook_helper.h"// hook_log
#include "renderer_hook.h"// vr_measured_pod_extent
#include "imgui.h"

// ---------------------------------------------------------------------------------------------
// Entry points are resolved through the loader at runtime rather than linked. The SDK ships an
// MSVC import library and this DLL is MinGW; more importantly, a hook DLL that hard-links a
// runtime the user may not have would fail to load the game at all. Everything below degrades to
// a logged warning instead.
// ---------------------------------------------------------------------------------------------

static HMODULE g_loader = nullptr;
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr = nullptr;

static PFN_xrCreateInstance p_xrCreateInstance = nullptr;
static PFN_xrDestroyInstance p_xrDestroyInstance = nullptr;
static PFN_xrGetSystem p_xrGetSystem = nullptr;
static PFN_xrEnumerateViewConfigurationViews p_xrEnumerateViewConfigurationViews = nullptr;
static PFN_xrCreateSession p_xrCreateSession = nullptr;
static PFN_xrDestroySession p_xrDestroySession = nullptr;
static PFN_xrCreateReferenceSpace p_xrCreateReferenceSpace = nullptr;
static PFN_xrDestroySpace p_xrDestroySpace = nullptr;
static PFN_xrEnumerateSwapchainFormats p_xrEnumerateSwapchainFormats = nullptr;
static PFN_xrCreateSwapchain p_xrCreateSwapchain = nullptr;
static PFN_xrDestroySwapchain p_xrDestroySwapchain = nullptr;
static PFN_xrEnumerateSwapchainImages p_xrEnumerateSwapchainImages = nullptr;
static PFN_xrAcquireSwapchainImage p_xrAcquireSwapchainImage = nullptr;
static PFN_xrWaitSwapchainImage p_xrWaitSwapchainImage = nullptr;
static PFN_xrReleaseSwapchainImage p_xrReleaseSwapchainImage = nullptr;
static PFN_xrBeginSession p_xrBeginSession = nullptr;
static PFN_xrEndSession p_xrEndSession = nullptr;
static PFN_xrWaitFrame p_xrWaitFrame = nullptr;
static PFN_xrBeginFrame p_xrBeginFrame = nullptr;
static PFN_xrEndFrame p_xrEndFrame = nullptr;
static PFN_xrLocateViews p_xrLocateViews = nullptr;
static PFN_xrPollEvent p_xrPollEvent = nullptr;
static PFN_xrResultToString p_xrResultToString = nullptr;
static PFN_xrCreateActionSet p_xrCreateActionSet = nullptr;
static PFN_xrCreateAction p_xrCreateAction = nullptr;
static PFN_xrApplyHapticFeedback p_xrApplyHapticFeedback = nullptr;
static PFN_xrStringToPath p_xrStringToPath = nullptr;
static PFN_xrSuggestInteractionProfileBindings p_xrSuggestInteractionProfileBindings = nullptr;
static PFN_xrAttachSessionActionSets p_xrAttachSessionActionSets = nullptr;
static PFN_xrSyncActions p_xrSyncActions = nullptr;
static PFN_xrGetActionStateFloat p_xrGetActionStateFloat = nullptr;
static PFN_xrGetActionStateVector2f p_xrGetActionStateVector2f = nullptr;
static PFN_xrGetActionStateBoolean p_xrGetActionStateBoolean = nullptr;
static PFN_xrDestroyActionSet p_xrDestroyActionSet = nullptr;

// Controller input. OpenXR's action system is the reason moving off OpenVR was worth doing
// now: the equivalent there is the deprecated legacy API, which SteamVR may not even supply
// for Touch controllers.
static XrActionSet g_action_set = XR_NULL_HANDLE;
static XrAction g_act_steer = XR_NULL_HANDLE;  // right thumbstick, Vector2f
static XrAction g_act_throttle = XR_NULL_HANDLE;// right trigger, float
static XrAction g_act_brake = XR_NULL_HANDLE;   // left trigger, float
static XrAction g_act_boost = XR_NULL_HANDLE;   // A, bool
static XrAction g_act_cancel = XR_NULL_HANDLE;  // B, bool
static XrAction g_act_menu = XR_NULL_HANDLE;    // left menu, bool
static XrAction g_act_pitch = XR_NULL_HANDLE;   // right thumbstick, Vector2f
static XrAction g_act_view = XR_NULL_HANDLE;    // left stick click, bool
static XrAction g_act_lookback = XR_NULL_HANDLE;// right stick click, bool
static XrAction g_act_rollL = XR_NULL_HANDLE;   // left grip, float
static XrAction g_act_rollR = XR_NULL_HANDLE;   // right grip, float
static XrAction g_act_slide = XR_NULL_HANDLE;      // right thumbstick click, bool
static XrAction g_act_repairbtn = XR_NULL_HANDLE;  // Y, bool
static XrAction g_act_haptic = XR_NULL_HANDLE;     // both hands, vibration output
static bool g_actions_ready = false;

// Live values, surfaced in the panel so the mapping can be eyeballed before anything is
// wired into the game's input.
static float g_in_steer_x = 0.0f, g_in_steer_y = 0.0f;
static float g_in_throttle = 0.0f, g_in_brake = 0.0f;
static bool g_in_boost = false, g_in_cancel = false, g_in_menu = false;
static float g_in_pitch = 0.0f;
// Right thumbstick X. Unused for driving -- steering is the left stick -- but the front-end
// menus navigate left/right, and a player reaching for 'the stick' should not have to know
// which one the menu listens to.
static float g_in_pitch_x = 0.0f;
static bool g_in_view = false, g_in_lookback = false, g_in_repair = false;
static bool g_in_slide = false;
static bool g_in_rollL = false, g_in_rollR = false;

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_space = XR_NULL_HANDLE;
static XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;

static bool g_session_running = false;// between xrBeginSession and xrEndSession
static bool g_init_attempted = false;
static bool g_gave_up = false;

// Per-eye swapchain, plus the FBO we use to blit into whichever image is acquired.
struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<uint32_t> images;// GL texture names
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t acquired = 0;
    bool has_image = false;
};
static EyeSwapchain g_eye[2];

// Head-locked quad for all 2D. VIEW space means the runtime keeps it fixed relative to the
// head with no work from us, and re-projects it correctly per eye.
static XrSpace g_view_space = XR_NULL_HANDLE;
static EyeSwapchain g_quad;
static bool g_quad_has_content = false;

// The panel is WORLD-locked, not head-locked. VIEW space would rigidly attach it to the
// head, which is what made menus and cutscenes feel awful -- they followed every movement
// and could never be looked away from. Instead it is placed once in LOCAL space, in front of
// wherever the viewer was facing, and then stays put so you can look around it.
static XrPosef g_panel_pose{{0, 0, 0, 1}, {0, 0, -1.4f}};
static bool g_panel_placed = false;
static int g_panel_idle_frames = 0;
static GLuint g_blit_src_fbo = 0;// wraps the texture the renderer hands us
static GLuint g_blit_dst_fbo = 0;// wraps the acquired swapchain image

// Per-frame state, refreshed in vr_probe_update.
static XrFrameState g_frame_state{};
static XrView g_views[2]{};
static bool g_views_valid = false;
static bool g_frame_begun = false;
static int g_current_eye = VR_EYE_LEFT;
static bool g_submitted_this_frame = false;

struct VrState {
    bool pose_valid = false;
    float yaw_deg = 0.0f, pitch_deg = 0.0f, roll_deg = 0.0f;
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    unsigned long frames = 0, valid_frames = 0, submits = 0;
    char status[256] = "not initialised";
    char runtime_name[128] = "";

    bool submit_enabled = true;
    bool head_drives_camera = true;
    bool swap_eye_order = false;
    // 0.0 is the tested value; 1.0 was a default nobody has played with.
    float menu_shift = 0.0f;
    bool dump_eyes = false;
    // On by default now that the layer composite is correct. It was off while the composite
    // was landing in a dead branch, because enabling it then hid the HUD everywhere.
    bool hud_redirect = true;
    // 1.0 = the panel at its full panel_width. This became load-bearing when HUD size
    // started scaling the VR quad: at the old 0.70 default every VR user's panel would
    // silently shrink by 30% compared to before, since the setting previously did nothing
    // in the headset at all.
    // 1.11, adopted from the tuned config after months of play rather than picked. The
    // shipped archive contains no ini, so compiled defaults ARE what a new player gets.
    float hud_scale = 1.11f;
    // F5 overlay magnification in VR only; flat play keeps its native size.
    float overlay_scale = 1.65f;
    float cull_fov_boost = 2.0f;
    bool render_all_selectors = false;
    // On by default from v1.2. It writes swrRace_SteeringInput directly, which is the
    // route the game's own analog controllers use; the previous implementation wrote a raw
    // DirectInput axis array several stages upstream and never reached the pod, which is
    // why the toggle appeared to do nothing.
    // On by default. The override is now applied inside swrRace_UpdatePlayerControl, on the
    // line that consumes the value, so it cannot be overwritten by the game's own input
    // pass the way the previous attempt was. The arrow keys stay live for menus, so this
    // cannot take control away even if it misbehaves.
    bool analog_steering = true;
    bool suppress_flares = true;// screen-space flares cannot anchor to the world in VR
    // With the screen-space ones dropped, redraw the light streaks as real geometry in the world
    // so they sit on their lamp posts. Half-size in metres; a streetlight glow is about a metre.
    bool world_flares = true;
    float flare_size_m = 1.0f;
    // Degrees across, for lights far enough away that their real size would be sub-pixel.
    // Deliberately restrained: a big screen-filling starburst is what the 1999 sprites did,
    // and in a headset it reads as a flat overlay rather than a light out in the world. A
    // small bright point with a hint of glint holds up much better at 90 Hz with head motion.
    // 12 with the soft radial falloff: the quad is wide but the visible core is small, since
    // the cubed falloff means most of it is transparent. Tuned by eye in the headset.
    float flare_angular_deg = 12.0f;
    // Snow and rain, VR only. Bigger and less streaky than the flat defaults: thin fast
    // slivers alias badly at headset FOV and survive video compression poorly.
    float weather_size_mul = 1.0f;
    float weather_streak_mul = 0.5f;
    // Touch controller vibration on impacts. Unrelated to the game's DirectInput force
    // feedback screen, which only ever enumerated wheels and joysticks.
    // Run flat, ignoring VR entirely. Mirrors SWE1R_NO_VR=1, which does not survive an
    // elevated launch: Windows builds an elevated process with a fresh environment, so a
    // variable set in the launching shell never reaches the game.
    bool no_vr = false;
    // Per-frame diagnostics. Mirrors SWE1R_VR_VERBOSE=1, same reasoning.
    bool verbose = false;
    bool haptics = true;
    float haptic_strength = 0.7f;
    // speedLoss -> amplitude, through a square-root curve. Calibrated across three sessions:
    // peaks of 57, 70 and 99, so the real ceiling is around 100 -- not the 70 assumed at
    // first, which made 0.014 saturate from 71 upward and flatten most hard hits together.
    // At 0.010 a 100 lands at 1.0, 57 at 0.75, 25 at 0.50, 5 at 0.22. Impacts above ~100
    // still saturate, which is intended: the hardest hit available should feel maximal.
    float haptic_impact_scale = 0.010f;
    // Speed drop -> amplitude, same curve. Calibrated from a real session: drops reach ~446
    // on a hard crash, and at 0.010 everything above ~220 clamped at maximum, so a scrape and
    // a crash felt identical. At 0.0022 a full crash lands near 1.0, a solid hit near 0.47,
    // and a light scrape near 0.21 -- dynamics across the whole range.
    float haptic_wall_scale = 0.0022f;
    // Below this, a speed drop is ordinary braking or drag rather than an impact. Without a
    // deadband the controllers hum continuously. Provisional -- the logged peak calibrates
    // it, exactly as the pod-impact scale was calibrated.
    float haptic_wall_deadband = 1.5f;
    // --- Wheel / joystick steering -------------------------------------------------
    // Reads a raw DirectInput axis and drives steering from it directly, skipping the
    // game's own axis binding. -1 disables it. Which axis a given wheel lands on is not
    // predictable, so the panel shows every axis live and the user picks the one that
    // moves. Off by default: this is written without a wheel to test against.
    // Master switch for every wheel feature. OFF by default and deliberately not
    // autodetected: a pad's left stick is axis 0 and its buttons share the same indices a
    // wheel uses, so no runtime test can tell them apart. Guessing wrong breaks pad,
    // keyboard and headset users who never asked for wheel support at all.
    // 0 Auto (on only when a wheel is actually attached), 1 Always on, 2 Off.
    // Auto works because DirectInput reports device TYPE -- a driving device is not a
    // gamepad. The earlier attempt watched axis 0 for movement, which a pad's left stick
    // trips, and that is why it had to become a manual switch at the time.
    int wheel_mode = 0;
    // Defaults below are a complete Logitech G923 map, measured on one. Another wheel will
    // report different axis and button numbers, so they are still settings -- but a G923 user
    // only has to tick the switch, and anyone else has a working layout to adjust rather than
    // a blank form to fill in.
    int wheel_steer_axis = 0;
    // Stop the GAME acting on the joystick itself while we read its axes for steering.
    // With a wheel attached the pedals rest at full deflection, which the game treats as a
    // held input: menus scroll and confirm on their own and the keyboard appears dead.
    bool wheel_suppress_game_input = true;
    // Pedal axes. Unipolar and inverted: they rest at maximum and fall as pressed, so
    // they auto-calibrate on their own min/max rather than sharing the steering logic.
    // Which is throttle and which is brake cannot be told apart by watching them move,
    // so both are settings and can be swapped live.
    int wheel_throttle_axis = 2;
    int wheel_brake_axis = 5;
    float wheel_pedal_threshold = 0.15f;
    // Third pedal. 0 = Slide, 1 = Boost, 2 = Look back.
    int wheel_clutch_axis = -1;
    int wheel_clutch_action = 0;
    // D-pad: the four directions sit in one contiguous block, measured as
    // 272 Left, 273 Up, 274 Right, 275 Down, so one base index covers all four.
    int wheel_dpad_base = 272;// Left, Up, Right, Down at +0..+3
    // Six freely assignable buttons. Action ids below.
    //            L-pad  R-pad    A    B   LB   RB    X    Y    +    -  spare spare
    int wheel_btn_index[12] = {261, 260, 256, 257, 263, 262, 258, 259, 265, 264, -1, -1};
    // A is Boost+Confirm, the same double duty A has on a gamepad: the two never collide,
    // because you are either in a menu or racing. The two spares are for Start and Back,
    // whose indices this wheel has not yet reported.
    //                          RollL RollR B+C Back Charge Boost LookBk Repair Cam Slide
    int wheel_btn_action[12] = {8, 9, 10, 4, 7, 0, 2, 5, 6, 1, 4, 6};
    // Raw counts at full lock. 0 means auto: track the largest magnitude seen and scale
    // to that, which self-calibrates after one full turn in each direction.
    int wheel_range = 0;
    float wheel_deadzone = 0.05f;
    // Full steering is reached at this fraction of the wheel's measured travel. A G923
    // turns 900 degrees lock to lock and a podracer wants a quick input, so using the
    // whole span feels lifeless however well it is calibrated. 2.5 means full lock at
    // roughly 40% of the wheel's travel, or about 180 degrees each way.
    float wheel_sensitivity = 2.5f;
    bool wheel_invert = false;
    // Measured, not guessed: the pod's world-space bounds span 17 game units and a podracer is
    // about 7 m, so 2.4 units/m sizes the world correctly. The earlier 3.2 was picked by eye while
    // the stereo image was still broken and made everything about a third too small.
    float world_units_per_metre = 2.4f;
    // Where the 2D panel sits, in metres. Close and large by default: menus should fill most
    // of the view rather than float off in the distance.
    float panel_distance = 1.4f;
    float panel_width = 2.6f;
};
static VrState g_s;

// VR settings live in the same SW_RACER_RE.ini the mod uses for its own, under [vr]. Without
// this every tuning choice -- world scale, panel placement, HUD size -- is lost on exit and
// has to be re-dialled in the headset every session.
static const char *vr_ini_path(void) {
    static char path[MAX_PATH] = {0};
    if (path[0] == 0) {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash)
            slash[1] = 0;
        strncat(path, "SW_RACER_RE.ini", MAX_PATH - strlen(path) - 1);
    }
    return path;
}

static float vr_ini_get_f(const char *key, float fallback) {
    char buf[64] = {0};
    char def[64];
    snprintf(def, sizeof(def), "%.4f", fallback);
    GetPrivateProfileStringA("vr", key, def, buf, sizeof(buf), vr_ini_path());
    return (float) atof(buf);
}

static bool vr_ini_get_b(const char *key, bool fallback) {
    return GetPrivateProfileIntA("vr", key, fallback ? 1 : 0, vr_ini_path()) != 0;
}

static void vr_ini_set_f(const char *key, float v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f", v);
    WritePrivateProfileStringA("vr", key, buf, vr_ini_path());
}

static void vr_ini_set_b(const char *key, bool v) {
    WritePrivateProfileStringA("vr", key, v ? "1" : "0", vr_ini_path());
}

static void vr_settings_load(void) {
    g_s.world_units_per_metre = vr_ini_get_f("world_units_per_metre", g_s.world_units_per_metre);
    g_s.panel_distance = vr_ini_get_f("panel_distance", g_s.panel_distance);
    g_s.panel_width = vr_ini_get_f("panel_width", g_s.panel_width);
    g_s.hud_scale = vr_ini_get_f("hud_scale", g_s.hud_scale);
    g_s.overlay_scale = vr_ini_get_f("overlay_scale", g_s.overlay_scale);
    g_s.cull_fov_boost = vr_ini_get_f("cull_fov_boost", g_s.cull_fov_boost);
    g_s.menu_shift = vr_ini_get_f("menu_shift", g_s.menu_shift);
    g_s.hud_redirect = vr_ini_get_b("hud_redirect", g_s.hud_redirect);
    // NOTE the key name. Everyone who ran v1.0/v1.1 has "analog_steering=0" persisted from
    // when the setting was a broken experiment, and a stale persisted value silently beats a
    // new compiled default -- so changing the default alone would reach nobody. The key is
    // versioned instead; the old one is left inert in their ini.
    g_s.analog_steering = vr_ini_get_b("analog_steering2", g_s.analog_steering);
    g_s.suppress_flares = vr_ini_get_b("suppress_flares", g_s.suppress_flares);
    g_s.world_flares = vr_ini_get_b("world_flares", g_s.world_flares);
    g_s.flare_size_m = vr_ini_get_f("flare_size_m", g_s.flare_size_m);
    g_s.flare_angular_deg = vr_ini_get_f("flare_angular_deg", g_s.flare_angular_deg);
    g_s.weather_size_mul = vr_ini_get_f("weather_size_mul", g_s.weather_size_mul);
    g_s.weather_streak_mul = vr_ini_get_f("weather_streak_mul", g_s.weather_streak_mul);
    g_s.render_all_selectors = vr_ini_get_b("render_all_selectors", g_s.render_all_selectors);
    g_s.no_vr = vr_ini_get_b("no_vr", g_s.no_vr);
    g_s.verbose = vr_ini_get_b("verbose", g_s.verbose);
    g_s.haptics = vr_ini_get_b("haptics", g_s.haptics);
    g_s.haptic_strength = vr_ini_get_f("haptic_strength", g_s.haptic_strength);
    g_s.haptic_impact_scale = vr_ini_get_f("haptic_impact_scale", g_s.haptic_impact_scale);
    g_s.haptic_wall_scale = vr_ini_get_f("haptic_wall_scale", g_s.haptic_wall_scale);
    g_s.haptic_wall_deadband = vr_ini_get_f("haptic_wall_deadband", g_s.haptic_wall_deadband);
    g_s.wheel_mode = (int) vr_ini_get_f("wheel_mode", (float) g_s.wheel_mode);
    g_s.wheel_steer_axis = (int) vr_ini_get_f("wheel_steer_axis", (float) g_s.wheel_steer_axis);
    g_s.wheel_suppress_game_input =
        vr_ini_get_b("wheel_suppress_game_input", g_s.wheel_suppress_game_input);
    g_s.wheel_throttle_axis =
        (int) vr_ini_get_f("wheel_throttle_axis", (float) g_s.wheel_throttle_axis);
    g_s.wheel_brake_axis = (int) vr_ini_get_f("wheel_brake_axis", (float) g_s.wheel_brake_axis);
    g_s.wheel_pedal_threshold =
        vr_ini_get_f("wheel_pedal_threshold", g_s.wheel_pedal_threshold);
    g_s.wheel_clutch_axis =
        (int) vr_ini_get_f("wheel_clutch_axis", (float) g_s.wheel_clutch_axis);
    g_s.wheel_clutch_action =
        (int) vr_ini_get_f("wheel_clutch_action", (float) g_s.wheel_clutch_action);
    g_s.wheel_dpad_base = (int) vr_ini_get_f("wheel_dpad_base", (float) g_s.wheel_dpad_base);
    for (int i = 0; i < 12; i++) {
        char k[40];
        snprintf(k, sizeof(k), "wheel_btn%d_index", i + 1);
        g_s.wheel_btn_index[i] = (int) vr_ini_get_f(k, (float) g_s.wheel_btn_index[i]);
        snprintf(k, sizeof(k), "wheel_btn%d_action", i + 1);
        g_s.wheel_btn_action[i] = (int) vr_ini_get_f(k, (float) g_s.wheel_btn_action[i]);
    }
    g_s.wheel_range = (int) vr_ini_get_f("wheel_range", (float) g_s.wheel_range);
    g_s.wheel_deadzone = vr_ini_get_f("wheel_deadzone", g_s.wheel_deadzone);
    g_s.wheel_sensitivity = vr_ini_get_f("wheel_sensitivity", g_s.wheel_sensitivity);
    g_s.wheel_invert = vr_ini_get_b("wheel_invert", g_s.wheel_invert);
}

void vr_settings_save(void) {
    vr_ini_set_f("world_units_per_metre", g_s.world_units_per_metre);
    vr_ini_set_f("panel_distance", g_s.panel_distance);
    vr_ini_set_f("panel_width", g_s.panel_width);
    vr_ini_set_f("hud_scale", g_s.hud_scale);
    vr_ini_set_f("overlay_scale", g_s.overlay_scale);
    vr_ini_set_f("cull_fov_boost", g_s.cull_fov_boost);
    vr_ini_set_f("menu_shift", g_s.menu_shift);
    vr_ini_set_b("world_flares", g_s.world_flares);
    vr_ini_set_f("flare_size_m", g_s.flare_size_m);
    vr_ini_set_f("flare_angular_deg", g_s.flare_angular_deg);
    vr_ini_set_f("weather_size_mul", g_s.weather_size_mul);
    vr_ini_set_f("weather_streak_mul", g_s.weather_streak_mul);
    vr_ini_set_b("hud_redirect", g_s.hud_redirect);
    vr_ini_set_b("analog_steering2", g_s.analog_steering);
    vr_ini_set_b("suppress_flares", g_s.suppress_flares);
    vr_ini_set_b("render_all_selectors", g_s.render_all_selectors);
    vr_ini_set_b("no_vr", g_s.no_vr);
    vr_ini_set_b("verbose", g_s.verbose);
    vr_ini_set_b("haptics", g_s.haptics);
    vr_ini_set_f("haptic_strength", g_s.haptic_strength);
    vr_ini_set_f("haptic_impact_scale", g_s.haptic_impact_scale);
    vr_ini_set_f("haptic_wall_scale", g_s.haptic_wall_scale);
    vr_ini_set_f("haptic_wall_deadband", g_s.haptic_wall_deadband);
    vr_ini_set_f("wheel_mode", (float) g_s.wheel_mode);
    vr_ini_set_f("wheel_steer_axis", (float) g_s.wheel_steer_axis);
    vr_ini_set_b("wheel_suppress_game_input", g_s.wheel_suppress_game_input);
    vr_ini_set_f("wheel_throttle_axis", (float) g_s.wheel_throttle_axis);
    vr_ini_set_f("wheel_brake_axis", (float) g_s.wheel_brake_axis);
    vr_ini_set_f("wheel_pedal_threshold", g_s.wheel_pedal_threshold);
    vr_ini_set_f("wheel_clutch_axis", (float) g_s.wheel_clutch_axis);
    vr_ini_set_f("wheel_clutch_action", (float) g_s.wheel_clutch_action);
    vr_ini_set_f("wheel_dpad_base", (float) g_s.wheel_dpad_base);
    for (int i = 0; i < 12; i++) {
        char k[40];
        snprintf(k, sizeof(k), "wheel_btn%d_index", i + 1);
        vr_ini_set_f(k, (float) g_s.wheel_btn_index[i]);
        snprintf(k, sizeof(k), "wheel_btn%d_action", i + 1);
        vr_ini_set_f(k, (float) g_s.wheel_btn_action[i]);
    }
    vr_ini_set_f("wheel_range", (float) g_s.wheel_range);
    vr_ini_set_f("wheel_deadzone", g_s.wheel_deadzone);
    vr_ini_set_f("wheel_sensitivity", g_s.wheel_sensitivity);
    vr_ini_set_b("wheel_invert", g_s.wheel_invert);
}

static void xr_logf(const char *fmt, ...) {
    if (!hook_log)
        return;
    va_list args;
    va_start(args, fmt);
    fprintf(hook_log, "[XR] ");
    vfprintf(hook_log, fmt, args);
    fprintf(hook_log, "\n");
    va_end(args);
    fflush(hook_log);
}

static void set_status(const char *s) {
    snprintf(g_s.status, sizeof(g_s.status), "%s", s);
}

static const char *xr_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (p_xrResultToString && g_instance != XR_NULL_HANDLE &&
        p_xrResultToString(g_instance, r, buf) == XR_SUCCESS)
        return buf;
    snprintf(buf, sizeof(buf), "XrResult %d", (int) r);
    return buf;
}

#define XR_CHECK(expr, what)                                                                       \
    do {                                                                                           \
        const XrResult _r = (expr);                                                                \
        if (XR_FAILED(_r)) {                                                                       \
            xr_logf("%s failed: %s", what, xr_str(_r));                                            \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static bool load_loader(void) {
    // Next to the exe, same as openvr_api.dll was.
    const char *candidates[] = {"openxr_loader.dll"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        g_loader = LoadLibraryA(candidates[i]);
        if (g_loader)
            break;
    }
    if (!g_loader) {
        xr_logf("openxr_loader.dll not found next to the exe. GetLastError=%lu", GetLastError());
        return false;
    }
    p_xrGetInstanceProcAddr =
        (PFN_xrGetInstanceProcAddr) GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) {
        xr_logf("openxr_loader.dll has no xrGetInstanceProcAddr");
        return false;
    }
    return true;
}

template <typename T> static bool resolve(XrInstance inst, const char *name, T *out) {
    PFN_xrVoidFunction fn = nullptr;
    const XrResult r = p_xrGetInstanceProcAddr(inst, name, &fn);
    if (XR_FAILED(r) || fn == nullptr) {
        xr_logf("could not resolve %s", name);
        return false;
    }
    *out = (T) fn;
    return true;
}

static bool create_instance(void) {
    if (!resolve(XR_NULL_HANDLE, "xrCreateInstance", &p_xrCreateInstance))
        return false;

    const char *exts[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "%s",
             "SWE1R PCVR");
    ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "%s",
             "dinput_hook");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    XR_CHECK(p_xrCreateInstance(&ci, &g_instance), "xrCreateInstance");

    resolve(g_instance, "xrResultToString", &p_xrResultToString);
    if (!resolve(g_instance, "xrDestroyInstance", &p_xrDestroyInstance) ||
        !resolve(g_instance, "xrGetSystem", &p_xrGetSystem) ||
        !resolve(g_instance, "xrEnumerateViewConfigurationViews",
                 &p_xrEnumerateViewConfigurationViews) ||
        !resolve(g_instance, "xrCreateSession", &p_xrCreateSession) ||
        !resolve(g_instance, "xrDestroySession", &p_xrDestroySession) ||
        !resolve(g_instance, "xrCreateReferenceSpace", &p_xrCreateReferenceSpace) ||
        !resolve(g_instance, "xrDestroySpace", &p_xrDestroySpace) ||
        !resolve(g_instance, "xrEnumerateSwapchainFormats", &p_xrEnumerateSwapchainFormats) ||
        !resolve(g_instance, "xrCreateSwapchain", &p_xrCreateSwapchain) ||
        !resolve(g_instance, "xrDestroySwapchain", &p_xrDestroySwapchain) ||
        !resolve(g_instance, "xrEnumerateSwapchainImages", &p_xrEnumerateSwapchainImages) ||
        !resolve(g_instance, "xrAcquireSwapchainImage", &p_xrAcquireSwapchainImage) ||
        !resolve(g_instance, "xrWaitSwapchainImage", &p_xrWaitSwapchainImage) ||
        !resolve(g_instance, "xrReleaseSwapchainImage", &p_xrReleaseSwapchainImage) ||
        !resolve(g_instance, "xrBeginSession", &p_xrBeginSession) ||
        !resolve(g_instance, "xrEndSession", &p_xrEndSession) ||
        !resolve(g_instance, "xrWaitFrame", &p_xrWaitFrame) ||
        !resolve(g_instance, "xrBeginFrame", &p_xrBeginFrame) ||
        !resolve(g_instance, "xrEndFrame", &p_xrEndFrame) ||
        !resolve(g_instance, "xrLocateViews", &p_xrLocateViews) ||
        !resolve(g_instance, "xrPollEvent", &p_xrPollEvent))
        return false;

    // Input is optional: if any of these are missing we still render, just without controls.
    resolve(g_instance, "xrCreateActionSet", &p_xrCreateActionSet);
    resolve(g_instance, "xrCreateAction", &p_xrCreateAction);
    resolve(g_instance, "xrApplyHapticFeedback", &p_xrApplyHapticFeedback);
    resolve(g_instance, "xrStringToPath", &p_xrStringToPath);
    resolve(g_instance, "xrSuggestInteractionProfileBindings",
            &p_xrSuggestInteractionProfileBindings);
    resolve(g_instance, "xrAttachSessionActionSets", &p_xrAttachSessionActionSets);
    resolve(g_instance, "xrSyncActions", &p_xrSyncActions);
    resolve(g_instance, "xrGetActionStateFloat", &p_xrGetActionStateFloat);
    resolve(g_instance, "xrGetActionStateVector2f", &p_xrGetActionStateVector2f);
    resolve(g_instance, "xrGetActionStateBoolean", &p_xrGetActionStateBoolean);
    resolve(g_instance, "xrDestroyActionSet", &p_xrDestroyActionSet);

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    PFN_xrGetInstanceProperties p_props = nullptr;
    if (resolve(g_instance, "xrGetInstanceProperties", &p_props) &&
        p_props(g_instance, &props) == XR_SUCCESS) {
        snprintf(g_s.runtime_name, sizeof(g_s.runtime_name), "%s", props.runtimeName);
        xr_logf("runtime: %s", props.runtimeName);
    }
    return true;
}

static void setup_actions(void);// defined below; called once the session exists

static bool create_session_and_swapchains(void) {
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(p_xrGetSystem(g_instance, &sgi, &g_system), "xrGetSystem");

    // Mandatory before session creation on the GL extension, even though we ignore the version
    // bounds: skipping it makes xrCreateSession fail on conformant runtimes.
    PFN_xrGetOpenGLGraphicsRequirementsKHR p_reqs = nullptr;
    if (resolve(g_instance, "xrGetOpenGLGraphicsRequirementsKHR", &p_reqs)) {
        XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        p_reqs(g_instance, g_system, &reqs);
    }

    uint32_t view_count = 0;
    XR_CHECK(p_xrEnumerateViewConfigurationViews(
                 g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count,
                 nullptr),
             "xrEnumerateViewConfigurationViews(count)");
    if (view_count != 2) {
        xr_logf("expected a 2-view stereo config, runtime reports %u", view_count);
        return false;
    }
    XrViewConfigurationView vcv[2]{};
    for (int i = 0; i < 2; i++)
        vcv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    XR_CHECK(p_xrEnumerateViewConfigurationViews(
                 g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count,
                 vcv),
             "xrEnumerateViewConfigurationViews");

    // The GL context must be current on this thread; that is why init is deferred to the first
    // frame rather than done at DLL attach.
    const HDC hdc = wglGetCurrentDC();
    const HGLRC hglrc = wglGetCurrentContext();
    if (hdc == nullptr || hglrc == nullptr) {
        xr_logf("no current GL context yet (hdc=%p hglrc=%p) -- deferring", (void *) hdc,
                (void *) hglrc);
        return false;
    }

    XrGraphicsBindingOpenGLWin32KHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    gb.hDC = hdc;
    gb.hGLRC = hglrc;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_system;
    XR_CHECK(p_xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    // LOCAL: seated, origin where the headset was when the runtime settled. We only consume
    // rotation plus the per-eye offset, so drift in position never moves the pilot out of the pod.
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(p_xrCreateReferenceSpace(g_session, &rsci, &g_space), "xrCreateReferenceSpace");

    uint32_t fmt_count = 0;
    XR_CHECK(p_xrEnumerateSwapchainFormats(g_session, 0, &fmt_count, nullptr),
             "xrEnumerateSwapchainFormats(count)");
    std::vector<int64_t> formats(fmt_count);
    XR_CHECK(p_xrEnumerateSwapchainFormats(g_session, fmt_count, &fmt_count, formats.data()),
             "xrEnumerateSwapchainFormats");

    // Prefer an sRGB format. The game's framebuffer already holds sRGB-encoded bytes, and a
    // NON-sRGB swapchain makes the runtime treat them as linear and apply its own linear->sRGB
    // conversion on the way to the display -- encoding them twice, which reads as washed-out,
    // milky colour in the headset while the monitor (which never goes through the runtime)
    // looks correct. Declaring sRGB tells the runtime the data is already encoded.
    //
    // The matching half is in the blit: GL would ALSO convert linear->sRGB when writing into an
    // sRGB target, so GL_FRAMEBUFFER_SRGB is disabled there to copy the bytes through as-is.
    int64_t chosen = formats.empty() ? 0 : formats[0];
    bool have_srgb = false;
    for (int64_t f: formats)
        if (f == GL_SRGB8_ALPHA8) {
            chosen = f;
            have_srgb = true;
            break;
        }
    if (!have_srgb)
        for (int64_t f: formats)
            if (f == GL_RGBA8) {
                chosen = f;
                break;
            }
    xr_logf("swapchain format 0x%llx (%s) from %u offered", (unsigned long long) chosen,
            chosen == GL_SRGB8_ALPHA8 ? "GL_SRGB8_ALPHA8"
                                      : (chosen == GL_RGBA8 ? "GL_RGBA8" : "runtime default"),
            fmt_count);

    for (int i = 0; i < 2; i++) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = chosen;
        ci.sampleCount = 1;
        ci.width = vcv[i].recommendedImageRectWidth;
        ci.height = vcv[i].recommendedImageRectHeight;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XR_CHECK(p_xrCreateSwapchain(g_session, &ci, &g_eye[i].handle), "xrCreateSwapchain");
        g_eye[i].width = ci.width;
        g_eye[i].height = ci.height;

        uint32_t img_count = 0;
        XR_CHECK(p_xrEnumerateSwapchainImages(g_eye[i].handle, 0, &img_count, nullptr),
                 "xrEnumerateSwapchainImages(count)");
        std::vector<XrSwapchainImageOpenGLKHR> imgs(img_count);
        for (uint32_t k = 0; k < img_count; k++)
            imgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        XR_CHECK(p_xrEnumerateSwapchainImages(g_eye[i].handle, img_count, &img_count,
                                              (XrSwapchainImageBaseHeader *) imgs.data()),
                 "xrEnumerateSwapchainImages");
        g_eye[i].images.clear();
        for (uint32_t k = 0; k < img_count; k++)
            g_eye[i].images.push_back(imgs[k].image);
        xr_logf("eye %d swapchain %ux%u, %u images", i, ci.width, ci.height, img_count);
    }

    // VIEW space: head-locked, for the 2D panel.
    XrReferenceSpaceCreateInfo vsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(p_xrCreateReferenceSpace(g_session, &vsci, &g_view_space)))
        xr_logf("VIEW reference space failed -- 2D panel will be disabled");

    {
        XrSwapchainCreateInfo qi{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        qi.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        qi.format = chosen;
        qi.sampleCount = 1;
        qi.width = 1920;
        qi.height = 1080;
        qi.faceCount = 1;
        qi.arraySize = 1;
        qi.mipCount = 1;
        if (XR_SUCCEEDED(p_xrCreateSwapchain(g_session, &qi, &g_quad.handle))) {
            g_quad.width = qi.width;
            g_quad.height = qi.height;
            uint32_t qn = 0;
            p_xrEnumerateSwapchainImages(g_quad.handle, 0, &qn, nullptr);
            std::vector<XrSwapchainImageOpenGLKHR> qimgs(qn);
            for (uint32_t k = 0; k < qn; k++)
                qimgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            p_xrEnumerateSwapchainImages(g_quad.handle, qn, &qn,
                                         (XrSwapchainImageBaseHeader *) qimgs.data());
            for (uint32_t k = 0; k < qn; k++)
                g_quad.images.push_back(qimgs[k].image);
            xr_logf("2D panel swapchain %ux%u, %u images", qi.width, qi.height, qn);
        } else {
            xr_logf("2D panel swapchain failed -- HUD will not be shown");
        }
    }

    setup_actions();

    if (g_blit_src_fbo == 0)
        glGenFramebuffers(1, &g_blit_src_fbo);
    if (g_blit_dst_fbo == 0)
        glGenFramebuffers(1, &g_blit_dst_fbo);

    return true;
}

static XrPath xr_path(const char *s) {
    XrPath p = XR_NULL_PATH;
    if (p_xrStringToPath)
        p_xrStringToPath(g_instance, s, &p);
    return p;
}

static XrAction make_action(XrActionType type, const char *name, const char *label) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    snprintf(ai.actionName, sizeof(ai.actionName), "%s", name);
    snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "%s", label);
    XrAction a = XR_NULL_HANDLE;
    if (p_xrCreateAction) {
        const XrResult r = p_xrCreateAction(g_action_set, &ai, &a);
        if (XR_FAILED(r)) {
            // This returning quietly cost a real bug. localizedActionName must be UNIQUE
            // within an action set; two actions here were both called "Boost", so the
            // second was rejected with XR_ERROR_LOCALIZED_NAME_DUPLICATED and the X button
            // did nothing for the entire life of the mod -- while the code read as though
            // it worked, because the poll loop simply skips null handles. A control that
            // fails to exist has to say so.
            xr_logf("xrCreateAction(%s / '%s') FAILED: %s -- that control will do nothing",
                    name, label, xr_str(r));
            return XR_NULL_HANDLE;
        }
    }
    return a;
}

// Bound against the Touch profile. A runtime is free to remap these, which is the point of
// the action system: we describe intent, the runtime decides which physical control serves it.
static void setup_actions(void) {
    if (!p_xrCreateActionSet || !p_xrAttachSessionActionSets || !p_xrStringToPath)
        return;

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof(si.actionSetName), "%s", "gameplay");
    snprintf(si.localizedActionSetName, sizeof(si.localizedActionSetName), "%s", "Gameplay");
    if (XR_FAILED(p_xrCreateActionSet(g_instance, &si, &g_action_set))) {
        xr_logf("xrCreateActionSet failed -- no controller input");
        return;
    }

    g_act_steer = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "steer", "Steer");
    g_act_throttle = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "throttle", "Throttle");
    g_act_brake = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "brake", "Brake");
    g_act_boost = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "boost", "Boost");
    g_act_cancel = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "cancel", "Cancel");
    g_act_menu = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");
    g_act_pitch = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "pitch", "Pitch");
    g_act_view = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "view", "Switch camera");
    g_act_lookback = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "lookback", "Look back");
    g_act_rollL = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "rollleft", "Roll left");
    g_act_rollR = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "rollright", "Roll right");
    g_act_slide = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "slide", "Slide");
    g_act_repairbtn = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "repairbtn", "Repair");
    g_act_haptic = make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Impact feedback");

    const XrActionSuggestedBinding binds[] = {
        {g_act_steer, xr_path("/user/hand/left/input/thumbstick")},
        {g_act_throttle, xr_path("/user/hand/right/input/trigger/value")},
        {g_act_brake, xr_path("/user/hand/left/input/trigger/value")},
        {g_act_boost, xr_path("/user/hand/right/input/a/click")},
        {g_act_cancel, xr_path("/user/hand/right/input/b/click")},
        {g_act_menu, xr_path("/user/hand/left/input/menu/click")},
        {g_act_pitch, xr_path("/user/hand/right/input/thumbstick")},
        {g_act_view, xr_path("/user/hand/left/input/thumbstick/click")},
        // Look Back is on X, which is reachable without letting go of anything. It was
        // free because its old action never got created (duplicate localized name).
        {g_act_lookback, xr_path("/user/hand/left/input/x/click")},
        {g_act_rollL, xr_path("/user/hand/left/input/squeeze/value")},
        {g_act_rollR, xr_path("/user/hand/right/input/squeeze/value")},
        {g_act_slide, xr_path("/user/hand/right/input/thumbstick/click")},
        {g_act_repairbtn, xr_path("/user/hand/left/input/y/click")},
        // Output actions are suggested in the same array as inputs. Both hands, no
        // subaction paths: a pod impact is not a left- or right-handed event, so one
        // xrApplyHapticFeedback call should reach every bound output.
        {g_act_haptic, xr_path("/user/hand/left/output/haptic")},
        {g_act_haptic, xr_path("/user/hand/right/output/haptic")},
    };

    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = xr_path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = binds;
    sb.countSuggestedBindings = (uint32_t) (sizeof(binds) / sizeof(binds[0]));
    const XrResult sr = p_xrSuggestInteractionProfileBindings(g_instance, &sb);
    if (XR_FAILED(sr)) {
        xr_logf("suggest bindings failed: %s", xr_str(sr));
        return;
    }

    XrSessionActionSetsAttachInfo ao{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ao.countActionSets = 1;
    ao.actionSets = &g_action_set;
    const XrResult ar = p_xrAttachSessionActionSets(g_session, &ao);
    if (XR_FAILED(ar)) {
        xr_logf("attach action sets failed: %s", xr_str(ar));
        return;
    }

    g_actions_ready = true;
    xr_logf("controller actions attached (Touch profile)");
}

static void try_init(void) {
    if (g_init_attempted)
        return;

    // Only attempt once the GL context exists; before that xrCreateSession cannot be satisfied.
    if (wglGetCurrentContext() == nullptr)
        return;

    g_init_attempted = true;
    xr_logf("---- PCVR (OpenXR backend) ----");
    vr_settings_load();

    char no_vr[8] = {0};
    const bool no_vr_env =
        GetEnvironmentVariableA("SWE1R_NO_VR", no_vr, sizeof(no_vr)) > 0 && no_vr[0] == '1';
    if (no_vr_env || g_s.no_vr) {
        xr_logf("skipping VR, running flat (%s)",
                no_vr_env ? "SWE1R_NO_VR=1" : "no_vr=1 in the [vr] ini block");
        set_status("VR disabled by setting");
        g_gave_up = true;
        return;
    }

    if (!load_loader() || !create_instance() || !create_session_and_swapchains()) {
        set_status("OpenXR init failed - running flat (see hook.log)");
        g_gave_up = true;
        return;
    }
    // Everything a bug report needs in one block: which runtime, what it asked for, and what
    // the machine actually is. Users paste hook.log; this is the top of it.
    xr_logf("=== environment ===");
    {
        const char *gl_r = (const char *) glGetString(GL_RENDERER);
        const char *gl_v = (const char *) glGetString(GL_VERSION);
        xr_logf("  gpu        : %s", gl_r ? gl_r : "(unknown)");
        xr_logf("  gl         : %s", gl_v ? gl_v : "(unknown)");
        xr_logf("  runtime    : %s", g_s.runtime_name[0] ? g_s.runtime_name : "(unknown)");
        xr_logf("  per-eye    : %ux%u", g_eye[0].width, g_eye[0].height);
        xr_logf("  2D panel   : %ux%u at %.2f m, %.2f m wide", g_quad.width, g_quad.height,
                g_s.panel_distance, g_s.panel_width);
        xr_logf("  world scale: %.2f units/m   cull boost: %.2fx", g_s.world_units_per_metre,
                g_s.cull_fov_boost);
        xr_logf("  hud layer  : %s   analog steering: %s",
                g_s.hud_redirect ? "on" : "off", g_s.analog_steering ? "on" : "off");
        xr_logf("  note       : env vars do NOT reach an elevated launch; the [vr] ini\n                keys no_vr / verbose do the same job");
        xr_logf("  haptics    : %s  strength %.2f  impact %.4f  wall %.4f",
                g_s.haptics ? "on" : "off", g_s.haptic_strength, g_s.haptic_impact_scale,
                g_s.haptic_wall_scale);
        xr_logf("  verbose    : set SWE1R_VR_VERBOSE=1 for per-frame diagnostics");
        xr_logf("===================");
    }

    set_status("session created - waiting for runtime to make it ready");
    xr_logf("session created; waiting for XR_SESSION_STATE_READY");
}

static void poll_events(void) {
    if (g_instance == XR_NULL_HANDLE)
        return;
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult r = p_xrPollEvent(g_instance, &ev);
        if (r != XR_SUCCESS)
            break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *ssc =
                (const XrEventDataSessionStateChanged *) &ev;
            g_session_state = ssc->state;
            xr_logf("session state -> %d", (int) g_session_state);

            if (g_session_state == XR_SESSION_STATE_READY && !g_session_running) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(p_xrBeginSession(g_session, &bi))) {
                    g_session_running = true;
                    set_status("running");
                    xr_logf("session begun");
                }
            } else if (g_session_state == XR_SESSION_STATE_STOPPING && g_session_running) {
                p_xrEndSession(g_session);
                g_session_running = false;
                set_status("session stopped by runtime");
            }
        }
    }
}

void vr_probe_init(void) {
    // Deliberately empty. OpenXR needs the GL context, so the real work happens on the first
    // frame in vr_probe_update. Nothing that can block runs during DLL attach any more.
    set_status("waiting for GL context");
}

void vr_probe_update(void) {
    if (g_gave_up)
        return;

    try_init();
    if (g_session == XR_NULL_HANDLE)
        return;

    poll_events();
}

// Opens the frame and locates the views. Called at the top of the scene render so the pass
// draws with this frame's pose; the OpenVR backend could do this at frame end because it
// submitted standalone textures, but OpenXR wants the rendering bracketed.
void vr_begin_frame(void) {
    if (g_gave_up || g_session == XR_NULL_HANDLE)
        return;
    if (g_frame_begun)
        return;// already opened this frame

    g_submitted_this_frame = false;
    g_views_valid = false;

    if (!g_session_running)
        return;

    g_s.frames++;

    g_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(p_xrWaitFrame(g_session, &fwi, &g_frame_state)))
        return;

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(p_xrBeginFrame(g_session, &fbi)))
        return;
    g_frame_begun = true;

    if (!g_frame_state.shouldRender)
        return;

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frame_state.predictedDisplayTime;
    vli.space = g_space;

    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t got = 0;
    for (int i = 0; i < 2; i++)
        g_views[i] = XrView{XR_TYPE_VIEW};
    if (XR_FAILED(p_xrLocateViews(g_session, &vli, &vs, 2, &got, g_views)) || got != 2)
        return;

    g_views_valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    g_s.pose_valid = g_views_valid;
    if (!g_views_valid)
        return;

    g_s.valid_frames++;

    // Diagnostic readout only; the camera path consumes the pose directly.
    const XrQuaternionf &q = g_views[0].pose.orientation;
    const float kR2D = 57.2957795f;
    g_s.yaw_deg = atan2f(2.0f * (q.w * q.y + q.x * q.z),
                         1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * kR2D;
    g_s.pitch_deg = asinf(fmaxf(-1.0f, fminf(1.0f, 2.0f * (q.w * q.x - q.y * q.z)))) * kR2D;
    g_s.roll_deg = atan2f(2.0f * (q.w * q.z + q.x * q.y),
                          1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * kR2D;
    g_s.pos_x = g_views[0].pose.position.x;
    g_s.pos_y = g_views[0].pose.position.y;
    g_s.pos_z = g_views[0].pose.position.z;

    vr_input_poll();

    if (g_s.valid_frames <= 5 || (g_s.valid_frames % 300) == 0)
        xr_logf("frame %lu  yaw %+7.2f pitch %+7.2f roll %+7.2f  pos [%+6.3f %+6.3f %+6.3f] m",
                g_s.frames, g_s.yaw_deg, g_s.pitch_deg, g_s.roll_deg, g_s.pos_x, g_s.pos_y,
                g_s.pos_z);
}

int vr_is_active(void) {
    return (!g_gave_up && g_session_running && g_views_valid && g_s.submit_enabled) ? 1 : 0;
}

void vr_get_target_size(unsigned int *width, unsigned int *height) {
    if (width)
        *width = g_eye[0].width;
    if (height)
        *height = g_eye[0].height;
}

int vr_eye_count(void) {
    return vr_is_active() ? 2 : 1;
}

int vr_eye_for_pass(int pass) {
    const int eye = (pass == 0) ? VR_EYE_LEFT : VR_EYE_RIGHT;
    if (!g_s.swap_eye_order)
        return eye;
    return (eye == VR_EYE_LEFT) ? VR_EYE_RIGHT : VR_EYE_LEFT;
}

void vr_set_current_eye(int eye) {
    g_current_eye = eye;
}

int vr_get_current_eye(void) {
    return g_current_eye;
}

int vr_get_eye_projection(int eye, float znear, float zfar, float *out16) {
    if (!out16 || !vr_is_active() || eye < 0 || eye > 1)
        return 0;

    // XrFovf gives signed half-angles in radians (left and down negative), so unlike OpenVR's raw
    // tangents there is no y-down convention to second-guess.
    const XrFovf &f = g_views[eye].fov;
    const float l = tanf(f.angleLeft);
    const float r = tanf(f.angleRight);
    const float u = tanf(f.angleUp);
    const float d = tanf(f.angleDown);

    for (int i = 0; i < 16; i++)
        out16[i] = 0.0f;
    out16[0] = 2.0f / (r - l);
    out16[5] = 2.0f / (u - d);
    out16[8] = (r + l) / (r - l);
    out16[9] = (u + d) / (u - d);
    out16[10] = -(zfar + znear) / (zfar - znear);
    out16[11] = -1.0f;
    out16[14] = -2.0f * zfar * znear / (zfar - znear);

    static bool logged[2] = {false, false};
    if (!logged[eye]) {
        logged[eye] = true;
        xr_logf("eye %d fov tangents l=%+.4f r=%+.4f u=%+.4f d=%+.4f", eye, l, r, u, d);
    }
    return 1;
}

void vr_get_eye_view(int eye, float *out16) {
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!out16)
        return;
    if (!vr_is_active() || !g_s.head_drives_camera || eye < 0 || eye > 1) {
        memcpy(out16, identity, sizeof(identity));
        return;
    }

    // The located pose already carries head rotation AND the eye's own offset, so unlike the
    // OpenVR backend there is no hand-rolled IPD: the runtime's per-eye pose IS the separation.
    const XrQuaternionf &q = g_views[eye].pose.orientation;
    const XrVector3f &p = g_views[eye].pose.position;

    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    // Eye->world rotation, column-major.
    float R[9];
    R[0] = 1 - 2 * (yy + zz);  R[1] = 2 * (xy + wz);      R[2] = 2 * (xz - wy);
    R[3] = 2 * (xy - wz);      R[4] = 1 - 2 * (xx + zz);  R[5] = 2 * (yz + wx);
    R[6] = 2 * (xz + wy);      R[7] = 2 * (yz - wx);      R[8] = 1 - 2 * (xx + yy);

    // World->eye is the transpose, with translation -R^T * p. Positions are metres; the game's
    // world is far larger, so scale them or the eyes end up effectively co-located.
    const float s = (g_s.world_units_per_metre > 0.01f) ? g_s.world_units_per_metre : 1.0f;
    const float px = p.x * s, py = p.y * s, pz = p.z * s;

    for (int i = 0; i < 16; i++)
        out16[i] = 0.0f;
    // out[col*4+row] = R^T[col][row] = R[row][col]
    out16[0] = R[0]; out16[4] = R[1]; out16[8] = R[2];
    out16[1] = R[3]; out16[5] = R[4]; out16[9] = R[5];
    out16[2] = R[6]; out16[6] = R[7]; out16[10] = R[8];
    out16[12] = -(R[0] * px + R[1] * py + R[2] * pz);
    out16[13] = -(R[3] * px + R[4] * py + R[5] * pz);
    out16[14] = -(R[6] * px + R[7] * py + R[8] * pz);
    out16[15] = 1.0f;
}

// Blit the renderer's finished eye texture into an acquired swapchain image. This replaces
// OpenVR's "hand the compositor your texture name" model: here the runtime owns the images.
void vr_submit_eye(int eye, unsigned int gl_color_texture) {
    if (!vr_is_active() || gl_color_texture == 0 || eye < 0 || eye > 1)
        return;
    EyeSwapchain &sc = g_eye[eye];
    if (sc.handle == XR_NULL_HANDLE || sc.images.empty())
        return;

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(p_xrAcquireSwapchainImage(sc.handle, &ai, &sc.acquired)))
        return;

    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(p_xrWaitSwapchainImage(sc.handle, &wi))) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(sc.handle, &ri);
        return;
    }

    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_blit_src_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint) gl_color_texture, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_blit_dst_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint) sc.images[sc.acquired], 0);
    const GLenum db = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &db);

    // Source is the renderer's screen-sized eye texture; destination is the runtime's requested
    // per-eye size. Both single-sample, so scaling with GL_LINEAR is allowed here.
    GLint src_w = 0, src_h = 0;
    glBindTexture(GL_TEXTURE_2D, (GLuint) gl_color_texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &src_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &src_h);

    glDisable(GL_SCISSOR_TEST);
    // Copy the bytes through unchanged. With an sRGB destination GL would otherwise apply its
    // own linear->sRGB encode on top of pixels that are already sRGB.
    const GLboolean srgb_was = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBlitFramebuffer(0, 0, src_w, src_h, 0, 0, (GLint) sc.width, (GLint) sc.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (srgb_was)
        glEnable(GL_FRAMEBUFFER_SRGB);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) prev_draw);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(sc.handle, &ri);

    sc.has_image = true;
    g_submitted_this_frame = true;
    g_s.submits++;
}

// Place the panel in front of the current head pose, yaw only so it stays upright and level
// however the head happens to be tilted at the time.
static void place_panel(void) {
    if (!g_views_valid)
        return;
    const XrPosef &h = g_views[0].pose;
    const XrQuaternionf &q = h.orientation;
    const float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z),
                             1.0f - 2.0f * (q.y * q.y + q.x * q.x));
    g_panel_pose.orientation = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    const float fx = -sinf(yaw);
    const float fz = -cosf(yaw);
    g_panel_pose.position = {h.position.x + fx * g_s.panel_distance, h.position.y,
                             h.position.z + fz * g_s.panel_distance};
    g_panel_placed = true;
    xr_logf("panel placed at [%+.2f %+.2f %+.2f] yaw %+.1f deg", g_panel_pose.position.x,
            g_panel_pose.position.y, g_panel_pose.position.z, yaw * 57.2957795f);
}

void vr_recenter_panel(void) {
    g_panel_placed = false;// re-placed on the next frame that has content
}

void vr_set_2d_layer(unsigned int gl_texture, int w, int h) {
    if (!vr_is_active() || gl_texture == 0 || g_quad.handle == XR_NULL_HANDLE ||
        g_quad.images.empty() || w <= 0 || h <= 0)
        return;

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_quad.handle, &ai, &g_quad.acquired)))
        return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(p_xrWaitSwapchainImage(g_quad.handle, &wi))) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        p_xrReleaseSwapchainImage(g_quad.handle, &ri);
        return;
    }

    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_blit_src_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint) gl_texture, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_blit_dst_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint) g_quad.images[g_quad.acquired], 0);
    const GLenum db = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &db);

    glDisable(GL_SCISSOR_TEST);
    const GLboolean srgb_was = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBlitFramebuffer(0, 0, w, h, 0, 0, (GLint) g_quad.width, (GLint) g_quad.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (srgb_was)
        glEnable(GL_FRAMEBUFFER_SRGB);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) prev_draw);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_quad.handle, &ri);
    g_quad_has_content = true;

    // Re-place whenever the panel returns after being away for a while: a menu opened from a
    // different heading should appear in front of you, not behind.
    if (!g_panel_placed || g_panel_idle_frames > 90)
        place_panel();
    g_panel_idle_frames = 0;
}

void vr_submit_both(unsigned int gl_color_texture) {
    vr_submit_eye(VR_EYE_LEFT, gl_color_texture);
    vr_submit_eye(VR_EYE_RIGHT, gl_color_texture);
}

int vr_did_submit_this_frame(void) {
    return g_submitted_this_frame ? 1 : 0;
}

// Closes the OpenXR frame. Unlike OpenVR, where each Submit stood alone, the runtime needs an
// xrEndFrame every frame that was begun -- including frames where we drew nothing, or it stalls.
void vr_frame_end(void) {
    if (!g_frame_begun || g_session == XR_NULL_HANDLE)
        return;

    XrCompositionLayerProjectionView pv[2]{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[2] = {nullptr, nullptr};
    uint32_t layer_count = 0;

    if (g_views_valid && g_eye[0].has_image && g_eye[1].has_image) {
        for (int i = 0; i < 2; i++) {
            pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[i].pose = g_views[i].pose;
            pv[i].fov = g_views[i].fov;
            pv[i].subImage.swapchain = g_eye[i].handle;
            pv[i].subImage.imageRect.offset = {0, 0};
            pv[i].subImage.imageRect.extent = {(int32_t) g_eye[i].width,
                                               (int32_t) g_eye[i].height};
            pv[i].subImage.imageArrayIndex = 0;
        }
        layer.space = g_space;
        layer.viewCount = 2;
        layer.views = pv;
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *) &layer;
    }

    // The 2D panel, head-locked in VIEW space and drawn over the scene. SOURCE_ALPHA blending
    // means the HUD's transparent regions let the game through, while an opaque cutscene
    // covers it -- both fall out of the same layer without special-casing.
    if (g_quad_has_content && g_view_space != XR_NULL_HANDLE &&
        g_quad.handle != XR_NULL_HANDLE) {
        const float aspect = (g_quad.height > 0)
                                 ? (float) g_quad.width / (float) g_quad.height
                                 : 1.777f;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        // LOCAL, not VIEW: world-locked so it stays where it was put.
        quad.space = g_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_quad.handle;
        quad.subImage.imageRect.offset = {0, 0};
        quad.subImage.imageRect.extent = {(int32_t) g_quad.width, (int32_t) g_quad.height};
        quad.subImage.imageArrayIndex = 0;
        quad.pose = g_panel_pose;
        // HUD size scales the quad here. It used to be applied ONLY in vr_hud_composite,
        // which targets the desktop mirror -- so the slider visibly worked on the monitor and
        // did nothing whatsoever in the headset, which is where it matters.
        //
        // The two paths cannot use the same mechanism: on a monitor the window is the frame
        // and the HUD is inset within it, whereas in VR the quad IS the frame, so the only
        // meaningful size change is the quad itself. Same perceived effect, different means.
        const float hud_k = vr_get_hud_scale();
        quad.size = {g_s.panel_width * hud_k, (g_s.panel_width * hud_k) / aspect};
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *) &quad;
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = g_frame_state.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layer_count;
    fei.layers = layer_count ? layers : nullptr;
    p_xrEndFrame(g_session, &fei);

    g_frame_begun = false;
    g_eye[0].has_image = false;
    g_eye[1].has_image = false;
    if (!g_quad_has_content)
        g_panel_idle_frames++;
    g_quad_has_content = false;
    g_submitted_this_frame = false;
}

void vr_probe_shutdown(void) {
    if (g_session != XR_NULL_HANDLE) {
        for (int i = 0; i < 2; i++)
            if (g_eye[i].handle != XR_NULL_HANDLE && p_xrDestroySwapchain)
                p_xrDestroySwapchain(g_eye[i].handle);
        if (g_space != XR_NULL_HANDLE && p_xrDestroySpace)
            p_xrDestroySpace(g_space);
        if (g_session_running && p_xrEndSession)
            p_xrEndSession(g_session);
        if (p_xrDestroySession)
            p_xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
    }
    if (g_instance != XR_NULL_HANDLE && p_xrDestroyInstance) {
        p_xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
    }
    xr_logf("shut down after %lu frames (%lu tracked, %lu submitted)", g_s.frames, g_s.valid_frames,
            g_s.submits);
}

// --- knobs the renderer and panel read -------------------------------------------------------

float vr_get_cull_fov_boost(void) {
    return g_s.cull_fov_boost;
}

int vr_render_all_selectors(void) {
    return (vr_is_active() && g_s.render_all_selectors) ? 1 : 0;
}

int vr_should_dump_eyes(void) {
    return g_s.dump_eyes ? 1 : 0;
}

void vr_clear_dump_eyes(void) {
    g_s.dump_eyes = false;
}

float vr_get_2d_u_shift(int eye) {
    if (g_s.menu_shift == 0.0f)
        return 0.0f;
    float proj[16];
    if (!vr_get_eye_projection(eye, 1.0f, 100.0f, proj))
        return 0.0f;
    return proj[8] * 0.5f * g_s.menu_shift;
}

int vr_suppress_flares(void) {
    return g_s.suppress_flares ? 1 : 0;
}

int vr_world_flares(void) {
    return g_s.world_flares ? 1 : 0;
}

float vr_weather_size_mul(void) {
    return g_s.weather_size_mul;
}

float vr_weather_streak_mul(void) {
    return g_s.weather_streak_mul;
}

float vr_flare_angular_tan(void) {
    // Half-angle, hence the 0.5. tanf of a small angle is cheap and exact enough here.
    return tanf(g_s.flare_angular_deg * 0.5f * 3.14159265f / 180.0f);
}

float vr_flare_size_units(void) {
    // The billboards live in game units; the setting is in metres so it stays meaningful
    // if the world scale is ever re-derived.
    return g_s.flare_size_m * ((g_s.world_units_per_metre > 0.01f)
                                   ? g_s.world_units_per_metre
                                   : 1.0f);
}

int vr_wheel_enabled(void) {
    if (g_s.wheel_mode == 1)
        return 1;// forced on
    if (g_s.wheel_mode == 2)
        return 0;// forced off
    return vr_wheel_device_present();// auto
}
int vr_wheel_steer_axis(void) {
    return vr_wheel_enabled() ? g_s.wheel_steer_axis : -1;
}
int vr_wheel_suppress_game_input(void) {
    return g_s.wheel_suppress_game_input ? 1 : 0;
}
int vr_wheel_throttle_axis(void) {
    return g_s.wheel_throttle_axis;
}
int vr_wheel_brake_axis(void) {
    return g_s.wheel_brake_axis;
}
float vr_wheel_pedal_threshold(void) {
    return g_s.wheel_pedal_threshold;
}
int vr_wheel_clutch_axis(void) {
    return g_s.wheel_clutch_axis;
}
int vr_wheel_clutch_action(void) {
    return g_s.wheel_clutch_action;
}
int vr_wheel_dpad_base(void) {
    return g_s.wheel_dpad_base;
}
int vr_wheel_btn_index(int slot) {
    return (slot >= 0 && slot < 12) ? g_s.wheel_btn_index[slot] : -1;
}
int vr_wheel_btn_action(int slot) {
    return (slot >= 0 && slot < 12) ? g_s.wheel_btn_action[slot] : 0;
}
// Bumped to ask the input layer to forget its learned wheel and pedal ranges.
static int g_wheel_recal = 0;
int vr_wheel_recal_generation(void) {
    return g_wheel_recal;
}
int vr_wheel_range(void) {
    return g_s.wheel_range;
}
float vr_wheel_deadzone(void) {
    return g_s.wheel_deadzone;
}
float vr_wheel_sensitivity(void) {
    return g_s.wheel_sensitivity;
}
int vr_wheel_invert(void) {
    return g_s.wheel_invert ? 1 : 0;
}

int vr_analog_steering_enabled(void) {
    return g_s.analog_steering ? 1 : 0;
}

int vr_input_available(void) {
    return (g_actions_ready && g_session_running) ? 1 : 0;
}
float vr_input_steer(void) {
    return g_in_steer_x;
}
float vr_input_stick_y(void) {
    return g_in_steer_y;
}
float vr_input_pitch(void) {
    return g_in_pitch;
}
float vr_input_pitch_x(void) {
    return g_in_pitch_x;
}

// --- Haptics ---------------------------------------------------------------------------
// The game's own Force Feedback screen enumerates DirectInput devices -- wheels and
// joysticks with motors -- so it correctly reports nothing for a headset. Touch controller
// vibration is an entirely separate OpenXR output path, which is this.
//
// Rate-limited: ResolvePodCollision runs every physics step, so an uncapped pulse would be
// a continuous buzz for as long as two pods are in contact rather than a hit.
static unsigned int g_haptic_next_ms = 0;      // pod impacts
static unsigned int g_haptic_wall_next_ms = 0; // wall/terrain, independent so neither masks
                                               // the other
static float g_haptic_peak_loss = 0.0f;
static float g_haptic_peak_push = 0.0f;

// Vibration is perceived compressively, so a linear map either clips every heavy hit or
// loses every light one -- the first build did the former, with all impacts pinned at max.
static float haptic_curve(float v, float scale) {
    const float x = v * scale;
    return (x <= 0.0f) ? 0.0f : sqrtf(x);
}

void vr_haptic_pulse(float amplitude, float duration_ms) {
    if (!g_s.haptics || g_act_haptic == XR_NULL_HANDLE || !p_xrApplyHapticFeedback ||
        g_session == XR_NULL_HANDLE)
        return;
    amplitude *= g_s.haptic_strength;
    if (amplitude <= 0.0f)
        return;
    if (amplitude > 1.0f)
        amplitude = 1.0f;
    if (duration_ms < 10.0f)
        duration_ms = 10.0f;

    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = amplitude;
    vib.duration = (XrDuration) (duration_ms * 1000000.0f);// XrDuration is nanoseconds
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = g_act_haptic;
    p_xrApplyHapticFeedback(g_session, &hi, (const XrHapticBaseHeader *) &vib);
}

// Pod-to-pod impact. speedLoss is the engine's own measure of how much the hit cost, and its
// scale is not documented anywhere -- so the peak seen is logged (once, on a new high water
// mark) to calibrate haptic_impact_scale from a real session rather than by guessing.
void vr_haptic_event(float amplitude, float duration_ms) {
    vr_haptic_pulse(amplitude, duration_ms);
}

void vr_haptic_impact(float speed_loss) {
    if (speed_loss <= 0.0f)
        return;
    if (speed_loss > g_haptic_peak_loss * 1.25f + 0.01f) {
        g_haptic_peak_loss = speed_loss;
        xr_logf("[haptic] peak pod impact speedLoss %.2f -> amplitude %.2f", speed_loss,
                haptic_curve(speed_loss, g_s.haptic_impact_scale));
    }
    const unsigned int now = GetTickCount();
    if (now < g_haptic_next_ms)
        return;
    g_haptic_next_ms = now + 90;
    vr_haptic_pulse(haptic_curve(speed_loss, g_s.haptic_impact_scale), 60.0f);
}

// Walls, terrain and crashes, driven by a frame-to-frame speed drop. Fires more often than
// a pod impact by nature, so it is shorter, quieter and on its own limiter. The deadband
// matters: ordinary deceleration produces a small drop every step, and without it the
// controllers would hum for the whole race.
void vr_haptic_wall(float push) {
    if (push < g_s.haptic_wall_deadband)
        return;
    if (push > g_haptic_peak_push * 1.25f + 0.01f) {
        g_haptic_peak_push = push;
        xr_logf("[haptic] peak wall/crash speed drop %.2f -> amplitude %.2f", push,
                haptic_curve(push, g_s.haptic_wall_scale));
    }
    const unsigned int now = GetTickCount();
    if (now < g_haptic_wall_next_ms)
        return;
    g_haptic_wall_next_ms = now + 70;
    vr_haptic_pulse(haptic_curve(push, g_s.haptic_wall_scale), 40.0f);
}
int vr_input_view(void) {
    return g_in_view ? 1 : 0;
}
int vr_input_lookback(void) {
    return g_in_lookback ? 1 : 0;
}
int vr_input_slide(void) {
    return g_in_slide ? 1 : 0;
}
int vr_input_repair(void) {
    return g_in_repair ? 1 : 0;
}
// Squeezing BOTH grips recentres the 2D panel. Rolling left and right at once is a
// contradiction no one performs on purpose, which makes it a free chord -- and it is
// reachable without taking a hand off the controls, unlike the button in this panel.
//
// It exists because the runtime's own recentre (holding the Meta button) does NOT fix a
// mis-placed panel: that shifts the whole LOCAL space, and the panel's stored pose shifts
// with it, so the panel keeps exactly the same wrong offset from your head.
static bool vr_both_grips(void) {
    return g_in_rollL && g_in_rollR;
}

void vr_poll_recenter_chord(void) {
    static bool was_chorded = false;
    const bool now = vr_both_grips();
    if (now && !was_chorded)
        vr_recenter_panel();
    was_chorded = now;
}

int vr_input_roll_left(void) {
    // Suppress both rolls while chorded so the game is not handed contradictory input.
    return (g_in_rollL && !vr_both_grips()) ? 1 : 0;
}
int vr_input_roll_right(void) {
    return (g_in_rollR && !vr_both_grips()) ? 1 : 0;
}
float vr_input_throttle(void) {
    return g_in_throttle;
}
float vr_input_brake(void) {
    return g_in_brake;
}
int vr_input_boost(void) {
    return g_in_boost ? 1 : 0;
}
int vr_input_cancel(void) {
    return g_in_cancel ? 1 : 0;
}
int vr_input_menu(void) {
    return g_in_menu ? 1 : 0;
}

float vr_overlay_scale(void) {
    return (g_s.overlay_scale > 0.2f) ? g_s.overlay_scale : 1.0f;
}

float vr_get_hud_scale(void) {
    return (g_s.hud_scale > 0.05f) ? g_s.hud_scale : 1.0f;
}

int vr_hud_redirect_enabled(void) {
    return g_s.hud_redirect ? 1 : 0;
}

void vr_input_poll(void) {
    if (!g_actions_ready || !g_session_running || !p_xrSyncActions)
        return;

    XrActiveActionSet aas{};
    aas.actionSet = g_action_set;
    aas.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO};
    sy.countActiveActionSets = 1;
    sy.activeActionSets = &aas;
    if (XR_FAILED(p_xrSyncActions(g_session, &sy)))
        return;

    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};

    if (g_act_steer != XR_NULL_HANDLE && p_xrGetActionStateVector2f) {
        gi.action = g_act_steer;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_session, &gi, &st)) && st.isActive) {
            g_in_steer_x = st.currentState.x;
            g_in_steer_y = st.currentState.y;
        }
    }
    struct FloatBind {
        XrAction act;
        float *out;
    };
    if (g_act_pitch != XR_NULL_HANDLE && p_xrGetActionStateVector2f) {
        gi.action = g_act_pitch;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_session, &gi, &st)) && st.isActive) {
            g_in_pitch = st.currentState.y;
            g_in_pitch_x = st.currentState.x;
        }
    }
    float rollL_v = 0.0f, rollR_v = 0.0f;
    const FloatBind fb[] = {{g_act_throttle, &g_in_throttle},
                            {g_act_brake, &g_in_brake},
                            {g_act_rollL, &rollL_v},
                            {g_act_rollR, &rollR_v}};
    for (const FloatBind &b: fb) {
        if (b.act == XR_NULL_HANDLE || !p_xrGetActionStateFloat)
            continue;
        gi.action = b.act;
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(p_xrGetActionStateFloat(g_session, &gi, &st)) && st.isActive)
            *b.out = st.currentState;
    }
    struct BoolBind {
        XrAction act;
        bool *out;
    };
    static bool ybtn = false;
    const BoolBind bb[] = {{g_act_boost, &g_in_boost},       {g_act_cancel, &g_in_cancel},
                           {g_act_menu, &g_in_menu},         {g_act_view, &g_in_view},
                           {g_act_lookback, &g_in_lookback}, {g_act_slide, &g_in_slide},
                           {g_act_repairbtn, &ybtn}};
    for (const BoolBind &b: bb) {
        if (b.act == XR_NULL_HANDLE || !p_xrGetActionStateBoolean)
            continue;
        gi.action = b.act;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(p_xrGetActionStateBoolean(g_session, &gi, &st)) && st.isActive)
            *b.out = st.currentState != 0;
    }

void vr_poll_recenter_chord(void);

    g_in_rollL = rollL_v > 0.5f;
    g_in_rollR = rollR_v > 0.5f;
    // Both grips at once: recentre the panel. Checked here, once per frame, right after the grip
    // states are refreshed.
    vr_poll_recenter_chord();
    // Boost is A alone. The old X-as-boost alias never worked (its action failed to
    // create), and it is not needed: the charge is held on the left stick and A only has
    // to be tapped, after which the throttle sustains it. X is Look Back now.
    g_in_repair = ybtn;

    // Log only while something is actually being pressed, so one short session shows whether
    // the bindings resolve without burying the log.
    static int logged = 0;
    static unsigned long t = 0;
    const bool active = fabsf(g_in_steer_x) > 0.15f || fabsf(g_in_steer_y) > 0.15f ||
                        g_in_throttle > 0.1f || g_in_brake > 0.1f || g_in_boost ||
                        g_in_cancel || g_in_menu;
    if (active && logged < 30 && (++t % 15) == 0) {
        logged++;
        xr_logf("input steer=[%+.2f %+.2f] throttle=%.2f brake=%.2f boost=%d cancel=%d menu=%d",
                g_in_steer_x, g_in_steer_y, g_in_throttle, g_in_brake, (int) g_in_boost,
                (int) g_in_cancel, (int) g_in_menu);
    }
}


// Wheel and pedal settings. Drawn in the INPUT panel, not the VR one: a wheel is input, and
// it works with no headset at all. Kept here because every setting it touches lives in this
// file's settings struct.
extern "C" void vr_draw_wheel_settings(void) {
    ImGui::SeparatorText("Wheel / joystick steering (experimental)");
    ImGui::Combo("Wheel support", &g_s.wheel_mode,
                 "Auto (on when a wheel is attached)\0Always on\0Off\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Auto asks DirectInput what each device IS, so a wheel switches this\n"
                          "on and a gamepad does not. Force it on only if your wheel reports\n"
                          "itself as something other than a driving device.");
    ImGui::SameLine();
    ImGui::TextDisabled(vr_wheel_device_present() ? "(wheel detected)" : "(no wheel seen)");
    ImGui::TextWrapped("Drives steering from a raw DirectInput axis, skipping the game's own\n"
                       "axis binding. Turn the wheel and watch which axis below moves, then\n"
                       "set that number. -1 is off.");
    ImGui::SliderInt("Steer axis", &g_s.wheel_steer_axis, -1, 14);
    ImGui::Checkbox("Stop the game reading the joystick itself",
                    &g_s.wheel_suppress_game_input);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wheel pedals rest at full deflection, which the game treats as\n"
                          "a held input -- menus move on their own and the keyboard seems\n"
                          "dead. This leaves the axes readable for steering but stops the\n"
                          "game acting on them. Turn off to use the game's own joystick\n"
                          "support instead.");
    ImGui::SliderFloat("Wheel deadzone", &g_s.wheel_deadzone, 0.0f, 0.30f, "%.2f");
    ImGui::SliderFloat("Wheel sensitivity", &g_s.wheel_sensitivity, 0.5f, 6.0f, "%.2fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Higher = full steering with less turn. 1.0 uses the wheel's\n"
                          "whole travel, which on a 900-degree wheel is very slow.");
    ImGui::SliderInt("Throttle axis", &g_s.wheel_throttle_axis, -1, 14);
    ImGui::SliderInt("Brake axis", &g_s.wheel_brake_axis, -1, 14);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("If the pedals are the wrong way round, swap these two numbers.");
    ImGui::SliderFloat("Pedal threshold", &g_s.wheel_pedal_threshold, 0.02f, 0.60f, "%.2f");
    ImGui::SliderInt("Clutch axis", &g_s.wheel_clutch_axis, -1, 14);
    ImGui::SliderInt("D-pad base index", &g_s.wheel_dpad_base, -1, 520);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The four directions are consecutive from here:\n"
                          "base+0 Left, +1 Up, +2 Right, +3 Down. 272 on a G923.");
    for (int i = 0; i < 12; i++) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "Button %d index", i + 1);
        ImGui::SliderInt(lbl, &g_s.wheel_btn_index[i], -1, 520);
        snprintf(lbl, sizeof(lbl), "Button %d does", i + 1);
        ImGui::Combo(lbl, &g_s.wheel_btn_action[i],
                     "Boost\0Slide\0Look back\0Confirm\0Back / pause\0Repair\0Camera\0"
                     "Charge boost (hold)\0Roll left\0Roll right\0Boost + Confirm\0");
        if (i == 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Charge boost is the game's hold-up-to-charge input. Put it on a\n"
                              "button you can hold while steering, then fire with Boost. It\n"
                              "pitches the nose down while held - that is the game's own trade,\n"
                              "not something the mod adds.");
    }
    ImGui::Combo("Clutch does", &g_s.wheel_clutch_action, "Slide\0Boost\0Look back\0");
    if (ImGui::Button("Recalibrate wheel and pedals")) {
        g_wheel_recal++;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("then turn lock to lock and floor each pedal");
    ImGui::Checkbox("Invert wheel", &g_s.wheel_invert);
    ImGui::SliderInt("Range (0 = auto)", &g_s.wheel_range, 0, 65535);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Raw counts at full lock. Auto scales to the largest value seen,\n"
                          "so one full turn each way calibrates it.");
    {
        // Only axes that are actually moving, so the list stays readable in a headset --
        // a device with 15 dead axes would otherwise bury the one that matters.
        static int seen[15] = {};
        int shown = 0;
        for (int i = 0; i < 15; i++) {
            const int v = vr_raw_axis(i);
            const int mag = v < 0 ? -v : v;
            if (mag > seen[i])
                seen[i] = mag;
            if (seen[i] > 64 || i == g_s.wheel_steer_axis) {
                ImGui::Text("axis %2d: %8d   (peak %d)%s", i, v, seen[i],
                            i == g_s.wheel_steer_axis ? "  <- selected" : "");
                shown++;
            }
        }
        if (shown == 0)
            ImGui::TextDisabled("no axis has moved yet - turn the wheel");
    }
}

void vr_probe_draw_imgui(void) {
    // Snapshot, compare at the end, write only on change: no Save button to forget, and no
    // ini write every frame.
    const VrState before = g_s;
    ImGui::TextUnformatted("OpenXR backend");
    ImGui::Separator();
    ImGui::Text("runtime   : %s", g_s.runtime_name[0] ? g_s.runtime_name : "(unknown)");
    ImGui::Text("status    : %s", g_s.status);
    ImGui::Text("session   : %s  state=%d", g_session_running ? "running" : "not running",
                (int) g_session_state);
    ImGui::Text("pose valid: %s", g_s.pose_valid ? "yes" : "no");
    ImGui::Text("target    : %ux%u per eye", g_eye[0].width, g_eye[0].height);
    ImGui::Text("frames    : %lu  tracked %lu  submitted %lu", g_s.frames, g_s.valid_frames,
                g_s.submits);
    ImGui::Separator();
    ImGui::Text("yaw %+7.2f  pitch %+7.2f  roll %+7.2f", g_s.yaw_deg, g_s.pitch_deg, g_s.roll_deg);
    ImGui::Text("pos %+6.3f %+6.3f %+6.3f m", g_s.pos_x, g_s.pos_y, g_s.pos_z);
    ImGui::Separator();
    ImGui::SliderFloat("World units per metre", &g_s.world_units_per_metre, 1.0f, 200.0f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    {
        // Turn the scale into something checkable. A podracer is about 7 m long, so adjust the
        // slider until this reads roughly that and the world is correctly sized -- far easier
        // to judge than stereo separation.
        const float units = vr_measured_pod_extent();
        const float dist = vr_measured_pod_distance();
        if (units > 0.0f) {
            const float scale =
                (g_s.world_units_per_metre > 0.01f) ? g_s.world_units_per_metre : 1.0f;
            ImGui::Text("pod spans %.0f units = %.1f m  (a podracer is ~7 m)", units,
                        units / scale);
            ImGui::Text("  -> %.1f units/m would make it 7 m", units / 7.0f);
            // Second, independent check: in cockpit view the engines are a few metres ahead.
            ImGui::Text("eye to pod %.0f units = %.1f m", dist, dist / scale);
        } else {
            ImGui::TextDisabled("pod not measured yet - enter a race");
        }
    }
    ImGui::SliderFloat("Menu/cutscene shift", &g_s.menu_shift, -2.0f, 2.0f, "%.2f");
    ImGui::Checkbox("HUD into VR layer (experimental)", &g_s.hud_redirect);
    if (ImGui::Button("Recenter panel"))
        vr_recenter_panel();
    ImGui::SliderFloat("Panel distance (m)", &g_s.panel_distance, 0.5f, 5.0f, "%.2f");
    ImGui::SliderFloat("Panel width (m)", &g_s.panel_width, 0.5f, 6.0f, "%.2f");
    ImGui::TextDisabled("Panel is world-locked: it stays put so you can look around it.\n"
                        "Recenter puts it back in front of you.");
    ImGui::SliderFloat("HUD size", &g_s.hud_scale, 0.30f, 2.00f, "%.2f");
    ImGui::SliderFloat("Overlay text size", &g_s.overlay_scale, 0.8f, 3.5f, "%.2fx");
    ImGui::TextDisabled("Size of THIS panel's text. Takes effect next frame.");
    ImGui::SliderFloat("Engine cull FOV boost", &g_s.cull_fov_boost, 1.0f, 3.0f, "%.2fx");
    ImGui::Checkbox("Draw all selector children", &g_s.render_all_selectors);
    ImGui::Checkbox("Swap eye render order", &g_s.swap_eye_order);
    ImGui::Checkbox("Head rotation drives camera", &g_s.head_drives_camera);
    ImGui::Checkbox("Submit to headset", &g_s.submit_enabled);
    ImGui::Separator();
    ImGui::Text("controller actions: %s", g_actions_ready ? "attached" : "NOT attached");
    ImGui::Text("steer  %+.2f %+.2f", g_in_steer_x, g_in_steer_y);
    ImGui::Text("thr %.2f  brk %.2f  boost %d cancel %d menu %d", g_in_throttle, g_in_brake,
                (int) g_in_boost, (int) g_in_cancel, (int) g_in_menu);
    ImGui::Checkbox("Analog steering (experimental)", &g_s.analog_steering);
    ImGui::Checkbox("Suppress the game's screen-space flares", &g_s.suppress_flares);
    ImGui::Checkbox("Draw flares in world space (VR)", &g_s.world_flares);
    ImGui::SliderFloat("Flare size (deg)", &g_s.flare_angular_deg, 0.2f, 12.0f, "%.2f");
    ImGui::TextDisabled("How wide a flare looks, in degrees. The moon is about\n"
                        "0.5 deg across, for reference.");
    ImGui::SliderFloat("Snow/rain size", &g_s.weather_size_mul, 0.5f, 6.0f, "%.2fx");
    ImGui::SliderFloat("Snow/rain streak", &g_s.weather_streak_mul, 0.0f, 1.5f, "%.2fx");
    ImGui::Separator();
    ImGui::Checkbox("Verbose logging (per-frame diagnostics)", &g_s.verbose);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Same as SWE1R_VR_VERBOSE=1, but survives an elevated launch,\n"
                          "which strips the environment. Takes effect next run.");
    ImGui::Checkbox("Controller haptics on impact", &g_s.haptics);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Touch controller vibration. Nothing to do with the game's own\n"
                          "Force Feedback screen, which only lists DirectInput wheels and\n"
                          "joysticks and correctly reports none for a headset.");
    ImGui::SliderFloat("Haptic strength", &g_s.haptic_strength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Haptic impact scale", &g_s.haptic_impact_scale, 0.002f, 0.050f, "%.4f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pod-to-pod hits. Amplitude is sqrt(speedLoss * scale), so light\n"
                          "taps stay light and heavy hits do not all clamp at maximum.\n"
                          "hook.log records the peak seen each session.");
    ImGui::SliderFloat("Haptic wall scale", &g_s.haptic_wall_scale, 0.0005f, 0.020f, "%.4f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Walls, terrain and crashes, from a sudden speed drop.");
    ImGui::SliderFloat("Wall deadband", &g_s.haptic_wall_deadband, 0.1f, 20.0f, "%.2f");

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Speed drop below this is treated as braking, not an impact.\n"
                          "Raise it if the controllers hum while simply slowing down.");
    ImGui::TextDisabled("Particles were tuned for a monitor. Streak 0 makes them\n"
                        "round flakes; 1 is the flat game's streaking.");
    if (g_s.world_flares && !g_s.suppress_flares)
        ImGui::TextDisabled("World-space flares need suppression ON, or you get both.");
    ImGui::TextDisabled("Screen-space flares are drawn on the flat panel and cannot\n"
                        "sit on the light they belong to. Off restores them.");
    ImGui::TextDisabled("Puts the game in joystick mode so the stick is a real axis\n"
                        "instead of arrow keys. Untick if steering breaks.");
    if (ImGui::Button("Dump both eye images"))
        g_s.dump_eyes = true;

    if (memcmp(&before.world_units_per_metre, &g_s.world_units_per_metre, sizeof(float)) != 0 ||
        before.panel_distance != g_s.panel_distance || before.panel_width != g_s.panel_width ||
        before.hud_scale != g_s.hud_scale || before.cull_fov_boost != g_s.cull_fov_boost ||
        memcmp(&before.overlay_scale, &g_s.overlay_scale, sizeof(float)) != 0 ||
        before.menu_shift != g_s.menu_shift || before.hud_redirect != g_s.hud_redirect ||
        before.analog_steering != g_s.analog_steering ||
        before.suppress_flares != g_s.suppress_flares ||
        before.world_flares != g_s.world_flares ||
        memcmp(&before.flare_size_m, &g_s.flare_size_m, sizeof(float)) != 0 ||
        memcmp(&before.flare_angular_deg, &g_s.flare_angular_deg, sizeof(float)) != 0 ||
        memcmp(&before.weather_size_mul, &g_s.weather_size_mul, sizeof(float)) != 0 ||
        memcmp(&before.weather_streak_mul, &g_s.weather_streak_mul, sizeof(float)) != 0 ||
        before.render_all_selectors != g_s.render_all_selectors) {
        vr_settings_save();
    }
}
