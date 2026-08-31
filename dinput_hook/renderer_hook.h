//
// Created by tly on 10.03.2024.
//

#pragma once

#include "imgui_utils.h"

extern GLuint default_framebuffer;

// The framebuffer 2D draws should go to, or 0 for the normal target. In VR every render-list
// draw (HUD, menus, text) is redirected into a transparent layer that is composited into both
// eyes at frame end -- otherwise none of it reaches the headset, because the 2D flush happens
// after both eyes have already been captured.
extern "C" GLuint vr_hud_layer_target(void);

// Non-zero while the SECOND eye is being drawn. The scene is rendered once per eye, so any
// per-frame simulation inside the render path would otherwise advance twice per frame.
extern "C" int vr_second_eye_pass(void);

// True on the FINAL eye pass of this frame (always true when VR is off). Anything that
// consumes a once-per-frame flag has to release it here, not on the first eye, or the later
// eyes get nothing.
extern "C" int vr_last_eye_pass(void);

// The VR 2D layer to draw the debug overlay into, so F5 is visible in the headset instead of
// on the monitor only. Returns 0 when there is no layer (VR off, or the redirect disabled),
// in which case the caller should draw to the default framebuffer as before.
extern "C" int vr_imgui_target(unsigned int *fbo, int *w, int *h);

// Longest axis of the local player's pod, in GAME UNITS. Divided by the pod's real length it
// gives the world scale directly, instead of judging stereo depth by eye -- which is a genuinely
// hard thing to eyeball and was picked while the image was still broken.
extern "C" float vr_measured_pod_extent(void);

// Distance from the eye to the pod, in GAME UNITS. In cockpit view the engines are a few
// metres ahead, so this cross-checks the scale independently of the pod's own bounds.
extern "C" float vr_measured_pod_distance(void);

// Drop the N64 mesh path's GL state shadows (bound program/texture, render mode, cull face,
// texture params). Must be called by any path that touches that GL state outside render_mesh --
// the glTF replacement draws call it since they can run mid-traversal between meshes.
void invalidate_mesh_gl_state_cache();

// True if an AABB (min xyz, max xyz, in the space mvp maps from) is completely outside the clip
// volume of mvp (row-vector convention, clip = v * mvp). Conservative: never culls a visible box.
bool aabb_outside_frustum(const float aabb[6], const rdMatrix44 &mvp);

extern "C" int stdDisplay_Update_Hook();

extern "C" void init_renderer_hooks();

// Drop a GL texture name from the LOD-deswizzle "already done" set when its texture is freed, so a
// reused name (after a track reload) is unscrambled again. Called from std3D_ClearTexture_delta.
void deswizzle_forget_texture(GLuint handle);
