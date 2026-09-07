// PCVR support for SWE1R.
//
// Milestone A (done): prove a 32-bit in-process OpenVR session can be established from inside
// dinput_hook and that HMD poses arrive every frame.
//
// Milestone B1 (here): get the game into the headset. The scene is still rendered ONCE, mono,
// and the same resolved image is submitted to both eyes -- so there is no stereo depth yet --
// but the head pose drives the camera and the compositor path is exercised end to end. B2
// replaces this with per-eye frusta and two scene traversals.
//
// OpenVR (not OpenXR) because the game and this DLL are 32-bit: SteamVR ships a genuine i686
// openvr_api.dll + vrclient.dll, but its only OpenXR loader is win64.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Called once from init_renderer_hooks(). Safe with no headset/SteamVR present -- it logs why
// it gave up and every later call becomes a no-op.
void vr_probe_init(void);

// Called once per frame from stdDisplay_Update_Hook(), before any rendering. Blocks in
// WaitGetPoses when we are a Scene app (that is the compositor's frame-pacing sync point) and
// refreshes the cached head pose.
void vr_probe_update(void);

void vr_probe_shutdown(void);

// 1 once a session is up AND submission is enabled. Guards every VR path in the renderer, so a
// missing headset leaves the flat game completely untouched.
int vr_is_active(void);

// Per-eye render target size the runtime asks for. Both eyes are the same size.
void vr_get_target_size(unsigned int *width, unsigned int *height);

// --- Stereo (Milestone B2) -------------------------------------------------------------------
// The scene is rendered once per eye. Submitting one image to both eyes does NOT fuse: the
// compositor applies each eye's own asymmetric off-axis projection to whatever it is handed, so
// an identical texture is displaced oppositely in each eye -- disparity matching no real depth.
// Correct stereo therefore needs a genuine per-eye frustum AND a per-eye view offset.

#define VR_EYE_LEFT 0
#define VR_EYE_RIGHT 1

// How many times the scene must be drawn this frame: 2 in VR, 1 flat.
int vr_eye_count(void);

// Which eye the renderer is currently drawing. Set by the render loop, read by the projection
// and view-matrix code.
void vr_set_current_eye(int eye);
int vr_get_current_eye(void);

// Per-eye projection, column-major, ready to drop into rdMatrix44. Uses the runtime's own
// asymmetric frustum. Returns 0 (and leaves out16 untouched) when VR is inactive, so the caller
// keeps the game's own projection.
int vr_get_eye_projection(int eye, float znear, float zfar, float *out_column_major_16);

// Combined head-to-eye * world-to-head, column-major, for pre-multiplying the game's view
// matrix. Carries the head rotation and the eye's IPD offset. Identity when inactive.
void vr_get_eye_view(int eye, float *out_column_major_16);

// Submit one already-resolved (non-multisample) OpenGL colour texture for one eye.
void vr_submit_eye(int eye, unsigned int gl_color_texture);

// Which eye to draw on pass 0 / pass 1. Normally left then right, but the F5 panel can swap the
// order: if a rendering artifact follows the SECOND pass it is state leaking between passes, and
// if it stays on the same eye it is that eye's projection. Diagnostic, and decisive.
int vr_eye_for_pass(int pass);

// True once any eye has been submitted this frame. Menus, cutscenes and the Smush player never
// reach the 3D scene path, so nothing gets submitted and the compositor just re-displays the
// last race frame -- which reads as the headset being frozen on glitched geometry. The frame-level
// fallback uses this to submit the flat desktop image instead.
int vr_did_submit_this_frame(void);

// Opens the runtime's frame and locates the eye poses. MUST be called before the scene is
// rendered: OpenXR expects rendering to sit inside xrBeginFrame/xrEndFrame, and locating the
// views first is what lets the pass draw with the CURRENT head pose instead of last frame's.
// Safe to call repeatedly; only the first call per frame does anything. No-op outside VR.
void vr_begin_frame(void);

// Call at the very end of the frame, after the fallback check. Clears the per-frame submit
// flag. This MUST NOT happen at the start of vr_probe_update(): the game calls
// stdDisplay_Update_Hook at the END of a frame, by which point swrViewport_Render has
// already submitted both eyes -- resetting there wipes the record of the stereo submission
// and makes the 2D fallback clobber it with a flat mono image (unfusable double vision).
void vr_frame_end(void);

// Multiplier for the ENGINE's culling/clipping fov while a VR pass renders. The engine
// builds its clip frustum from the pod camera's fov and knows nothing about head rotation,
// so geometry you look at by turning your head has often already been discarded before our
// renderer sees it. Widening that frustum costs some offscreen work and changes culling
// only -- the projection actually rendered comes from the runtime. 1.0 = untouched.
float vr_get_cull_fov_boost(void);

// Non-zero to force NODE_SELECTOR nodes to draw ALL their children instead of the single one
// the engine picked. SWE1R splits the track into sections and game code selects which are
// visible from the POD's viewpoint each frame (-2 = draw nothing); turning your head shows
// you sections it already decided to skip, so the ground vanishes. This is a blunt override:
// selectors are also used for damage states and LOD variants, so drawing every child can
// introduce overlapping geometry. Diagnostic first, scoped to track nodes if it works.
int vr_render_all_selectors(void);

// One-shot request to write both eye images to disk. Set from the F5 panel; the renderer
// clears it once it has written the files. Looking at the actual pixels beats reasoning
// about what should be in them.
int vr_should_dump_eyes(void);
void vr_clear_dump_eyes(void);

// Horizontal offset, in texture-width units, to apply to flat 2D content for one eye.
// Identical pixels in both eyes do NOT fuse: the compositor maps the texture across each
// eye's asymmetric frustum, whose principal point sits at proj[8] (+/-0.24 here), splitting
// the content into two images a quarter of a screen apart. Offsetting each eye by half that
// puts it back in one place. Scaled by the F5 'Menu/cutscene shift' slider.
float vr_get_2d_u_shift(int eye);

// Fraction of the view the 2D layer occupies, about the centre. The HUD is authored for a
// monitor, where the whole screen sits inside a ~50 degree cone; in a headset that same
// image spans well over 100 degrees and the corners fall outside where eyes comfortably
// reach. 1.0 = full size (original), smaller = pulled in toward the centre.
float vr_get_hud_scale(void);

// Text/UI magnification for the F5 overlay in VR. The overlay is laid out for a monitor an
// arm's length away; on a panel a metre and a half out it is legible but small.
float vr_overlay_scale(void);

// Poll the tracked controllers and log what they report. Uses the legacy GetControllerState
// rather than the IVRInput action system: it is deprecated, but it needs no action-manifest
// JSON and it is out-param based, so it is safe across the MinGW/MSVC struct-return ABI gap
// that crashed us earlier. If SteamVR declines to supply legacy state for Touch controllers
// this will report nothing, and the manifest route is the fallback.
void vr_input_poll(void);

// Live controller values, for feeding the game's input. Steering is the thumbstick X axis
// (-1..+1), throttle and brake are the triggers (0..1), the rest are buttons.
int vr_input_available(void);
float vr_input_steer(void);
float vr_input_throttle(void);
float vr_input_brake(void);
int vr_input_boost(void);
int vr_input_cancel(void);
int vr_input_menu(void);
float vr_input_stick_y(void);
float vr_input_pitch(void);   // right stick Y: nose up / down
float vr_input_pitch_x(void); // right stick X: menu left/right only
int vr_input_view(void);      // left stick click: switch camera
int vr_input_lookback(void);  // X: look back
int vr_input_slide(void);     // right stick click: slide
int vr_input_repair(void);    // Y: repair
int vr_input_roll_left(void); // left grip
int vr_input_roll_right(void);// right grip

// Touch controller vibration. Separate from the game's DirectInput force feedback, which
// enumerates wheels and joysticks and correctly finds none for a headset.
// amplitude 0..1 (scaled by the user's strength setting), duration in milliseconds.
void vr_haptic_pulse(float amplitude, float duration_ms);
// Pod-to-pod impact, driven by the engine's own speedLoss. Rate-limited internally --
// ResolvePodCollision runs every physics step, so an uncapped pulse is a buzz, not a hit.
void vr_haptic_impact(float speed_loss);
// Wall / terrain scrape, from swrRace.wallPushback. Own scale and rate limiter.
void vr_haptic_wall(float push);
// A one-off pulse at a given amplitude, for discrete events such as boost engaging.
void vr_haptic_event(float amplitude, float duration_ms);


// Wheel / joystick steering, read straight from a raw DirectInput axis and applied at the
// same point as the VR stick. -1 disables. Bypasses the game's own axis binding, which is
// the step a wheel appears to fall down on: the axes reach stdControl_aAxisPos, but do not
// reach the pod unless the profile binds them.
// Master switch. Every other wheel accessor reports 'off' when this is 0.
int vr_wheel_enabled(void);
// Restore the wheel's centring spring, which DirectInput disables on acquisition.
int vr_wheel_autocenter(void);
int vr_wheel_steer_axis(void);
int vr_wheel_range(void);    // raw counts at full lock; 0 = auto-calibrate from the peak
float vr_wheel_deadzone(void);
// Full steering at this fraction of the wheel's travel; higher is sharper.
float vr_wheel_sensitivity(void);
int vr_wheel_invert(void);
// Non-zero to stop the GAME acting on the joystick while we still read its axes.
int vr_wheel_suppress_game_input(void);
// Pedal axes: unipolar, resting at maximum and falling as pressed. -1 disables.
int vr_wheel_throttle_axis(void);
int vr_wheel_brake_axis(void);
float vr_wheel_pedal_threshold(void);
int vr_wheel_clutch_axis(void);
// 0 = Slide, 1 = Boost, 2 = Look back.
int vr_wheel_clutch_action(void);
// D-pad directions are consecutive from this index: +0 Left, +1 Up, +2 Right, +3 Down.
int vr_wheel_dpad_base(void);
// Six assignable buttons. Action: 0 Boost, 1 Slide, 2 Look back, 3 Confirm,
// 4 Back/pause, 5 Repair, 6 Camera, 7 Charge boost (the hold-up-to-charge input),
// 8 Roll left, 9 Roll right.
int vr_wheel_btn_index(int slot);
int vr_wheel_btn_action(int slot);
// Changes when the user asks for recalibration; the input layer forgets its learned
// ranges when this differs from the value it last saw.
int vr_wheel_recal_generation(void);
// One raw DirectInput axis (0..14), for the axis picker. Returns 0 out of range.
int vr_raw_axis(int i);

// Non-zero to put the game into joystick mode and drive it from the thumbstick as a real
// analog axis, instead of thresholding the stick into arrow-key presses. Quest controllers
// are not gamepads to Windows -- no XInput or DirectInput device exists for them -- so the
// game cannot see them directly either way; this decides which of the game's own input paths
// we translate them into. Off by default until proven.
int vr_analog_steering_enabled(void);

// Hand the frame's 2D content (HUD, menus, cutscenes) to the runtime as a HEAD-LOCKED quad
// layer, rather than compositing it into the eye textures.
//
// Compositing it into the eyes was wrong in two ways at once: the 2D inherited the
// head-tracked scene, so menus and cutscenes rode head movement instead of staying put in
// front of the viewer; and identical pixels in both eyes had to be shifted by hand to cancel
// the per-eye frustum asymmetry, which is what made movies double. A quad layer in VIEW space
// is head-locked by construction and the runtime handles the per-eye projection itself, so
// both problems disappear along with the shift correction.
void vr_set_2d_layer(unsigned int gl_texture, int w, int h);

// Re-place the world-locked 2D panel in front of wherever the viewer is now facing.
void vr_recenter_panel(void);

// Non-zero to suppress the game's screen-space lens flares and light streaks while in VR.
// They are computed as a 2D overlay from a projected world point and drawn in the HUD pass,
// which in VR lands them on the flat panel rather than out in the world -- so they can never
// sit on the light they belong to. Most VR ports drop screen-space flares for this reason.
int vr_suppress_flares(void);

// Redraw the suppressed light streaks as world-space billboards inside each eye pass.
int vr_world_flares(void);

// Billboard half-size in GAME UNITS, already converted from metres by the world scale.
float vr_flare_size_units(void);

// Tangent of the half-angle a DISTANT flare holds on screen, so far track lights stay
// visible instead of shrinking to nothing the way real geometry would.
float vr_flare_angular_tan(void);

// Weather particles were sized and streaked for a flat monitor. In a headset they read as
// tiny shimmering slivers -- a wide FOV plus streaming compression is unkind to thin, fast
// moving 1-2 pixel shapes. These scale size and streak length in VR only.
float vr_weather_size_mul(void);
float vr_weather_streak_mul(void);

// Whether to divert 2D drawing into the VR HUD layer. Defaults OFF: while the composite is
// unresolved, turning it on makes the HUD invisible EVERYWHERE, including the main menu on the
// monitor -- which leaves the game unplayable. Opt in from the F5 panel to test.
int vr_hud_redirect_enabled(void);

// Submit one texture to BOTH eyes, for the flat 2D fallback above. Not stereo, deliberately:
// menus and prerendered cutscenes have no depth to convey.
void vr_submit_both(unsigned int gl_color_texture);

#ifdef __cplusplus
}

// imgui panel body, drawn from the F5 debug menu.
void vr_probe_draw_imgui(void);
#endif
