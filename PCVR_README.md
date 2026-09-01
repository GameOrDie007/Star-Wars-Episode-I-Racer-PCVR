# Star Wars Episode I Racer PCVR

VR support for **Star Wars Episode I: Racer** (1999 PC release), built as a layer on top of the
[SW_RACER_RE](https://github.com/tim-tim707/SW_RACER_RE) community improvement mod.

Stereo rendering with true per-eye projection, head tracking, a world-locked 2D panel for HUD and
menus, and full Quest controller support - on a game with no available source, via a hook DLL.

---

## Requirements

**A 32-bit OpenXR runtime.** This is the one hard requirement and it rules some setups out:

| Runtime | Works? |
|---|---|
| **Virtual Desktop (VDXR)** | Yes - ships and registers a 32-bit runtime |
| SteamVR | **No** - its OpenXR loader is 64-bit only |
| Meta / Oculus PC runtime | Probably - documented as supporting 32-bit apps, untested here |

The game is a 32-bit executable, so the VR runtime must be too. If your active OpenXR runtime has no
32-bit half, the game launches normally and runs flat, with the reason written to `hook.log`.

To check: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` must exist and point at a
32-bit DLL.

Also required: the SW_RACER_RE community improvement mod, and a legitimate copy of the game (GOG or
Steam; developed against Steam).

## Installing

Drop these into the game folder, next to `SWEP1RCR.EXE`:

- `dinput.dll` - the mod plus the VR layer (~7 MB)
- `openxr_loader.dll` - 32-bit Khronos loader (~2 MB)
- `assets/` - shaders and HD fonts (~440 KB)

`assets/shaders/` is **required** - the renderer reads its shaders from there at startup and
will not draw without them. `assets/textures/` is optional: it holds the community mod's HD font
atlases and loading screens, and if it is missing the game quietly keeps its built-in fonts
(`hook.log` says so).

**Custom tracks are not included.** The community mod ships an `assets/custom_tracks/` folder of
user-made courses, around 217 MB, which has nothing to do with VR and is left out to keep this
download small. If you want them, take that folder from the
[SW_RACER_RE release](https://github.com/tim-tim707/SW_RACER_RE/releases) and drop it in beside
the others - the two sets of assets merge.

Start Virtual Desktop and connect your headset **before** launching the game.

**Nothing else needs installing.** `dinput.dll` is statically linked and imports only Windows system
libraries - no Visual C++ redistributable, no MinGW runtime. `openxr_loader.dll` needs only
kernel32 and advapi32.

**A stock Steam install of this game crashes on launch, and dropping these files in fixes it.**
Verified on a clean machine: fresh Steam install, launched vanilla, crashed; added the three items
above and it started and ran in VR. You do **not** need the `dinput.dll` that circulates on the
Steam forums - ours occupies the same slot and does the same job. A stock install ships no
`dinput.dll` of its own, so if there is one in your folder it is something you added, and ours
replaces it. Back it up first if you want a way back.

Launch from the game's own folder - the game resolves `data/` relative to the working directory and
will not start otherwise:

    Set-Location "<game folder>"; .\SWEP1RCR.EXE

`openvr_api.dll` is **not** needed. If you have one from an earlier build of this mod, it is dead
weight from the parked OpenVR backend and can be deleted.

## Controls

| Quest | Action |
|---|---|
| Left stick left / right | Turn |
| Right stick up / down | Nose down / pull up |
| Right trigger | Thrust |
| Left trigger | Brake |
| Left / right grip | Roll left / right |
| Left stick click | Switch camera |
| Right stick click | Look back |
| X | Boost |
| Y | Repair |
| A | Confirm (menus) |
| B | Slide while racing / **Back** in menus |
| Left menu | Pause / **Back** |
| **Both grips together** | **Recentre the 2D panel** |

**Either thumbstick navigates menus**, in both directions. Keyboard continues to work
alongside the controllers.

**There are two Back buttons.** B backs out of menus and the pause screen; the left menu
button does the same and also pauses a race. B keeps its Slide job while you are actually
driving, so it does not pause the game mid-race - the mod switches its meaning based on
whether a menu is on screen.

## Settings

**F5** opens the debug overlay. VR settings live under the **VR** section and are saved to a `[vr]`
block in `SW_RACER_RE.ini`, so they persist between sessions.

Worth tuning:

- **World units per metre** - how large the world feels. Affects stereo separation. The
  default 2.4 is derived from the pod's own dimensions rather than picked by eye: a podracer is
  about 7 m and measures 17 game units. Raise it to shrink the world, lower it to enlarge.
- **Panel distance / width** - where the 2D panel sits. It is world-locked, so it stays put and you
  can look around it; **Recenter panel**, or squeezing both grips, puts it back in front of you.

  Your headset's own recentre (holding the Meta button) will **not** fix a badly placed panel: it
  shifts the whole tracking space, and the panel's position shifts with it, keeping exactly the
  same wrong offset from your head. Use the grips.
- **HUD size** - how much of your view the 2D panel fills. 1.0 is the panel at its full *Panel
  width*; above that it reaches further into your periphery, below it pulls the corners in. The
  HUD and the menus share one layer, so this moves both - a size that suits racing can leave
  menus oversized. Separating them needs two layers and is not done.
- **Overlay text size** - magnifies this debug panel's own text, which is laid out for a monitor
  at arm's length and reads small on a panel a metre and a half away.
- **Engine cull FOV boost** - widens the engine's culling frustum so scenery does not vanish when
  you turn your head. Costs performance; lower it if you need frames.
- **Controller haptics on impact** - on by default. The Touch controllers buzz when your
  pod is hit, scaled by how hard. *Haptic strength* sets the overall level and *Haptic
  impact scale* how sharply a hit translates into vibration.

  This is unrelated to the game's own **Force Feedback** settings screen, which reports
  *NO FORCE FEEDBACK DEVICE DETECTED*. That screen enumerates DirectInput devices -
  steering wheels and joysticks with motors - and a headset is not one, so it says the same
  thing on an unmodded install. Nothing is wrong with your setup.
- **Suppress the game's screen-space flares** - on by default; drops the originals, which
  cannot anchor to the world in VR.
- **Draw flares in world space (VR)** - on by default. Redraws them as billboards that sit on
  their lamp posts and stay there as you turn your head.
- **Flare size (deg)** - how wide a flare appears, in degrees. Distant lights hold their
  angular size rather than shrinking to sub-pixel, the way the original sprites did. The moon
  is about 0.5 deg across, for reference.

For performance, in **Render -> Graphics Settings**: enable *Cull off-screen meshes*, disable *AI full
LOD*, and leave the frame cap unlimited.

## Known limitations

**Overhead place numbers are suppressed.** The game draws them as a screen-space effect: it
projects a 3D point to a 2D screen position and stamps a sprite there, during the HUD pass. In VR
that pass is a flat panel, so they cannot anchor to the world - the numbers drift off the racers as
you turn your head, which is worse than not having them.

The fix would be to run the game's own world-sprite pass *inside* the eye pass, before the eye image
is captured, so the existing VR-aware reprojection in `swrViewport_ProjectToScreen_delta` applies
per eye. That touches the working HUD path, so it is deliberately left undone.

**Lens flares are redrawn in world space** and do anchor correctly - see *Settings*. They are real
geometry in each eye, fogged with the engine's own fog and occluded by terrain via the depth buffer.
Note this is stricter than vanilla, whose occlusion test is broken at 24/32 bpp and lets flares shine
through walls, so you may see slightly fewer of them than the flat game shows.

**Frame rate is mid-to-high 80s on a 90 Hz headset** with strong hardware. The limit is CPU draw-call
submission in a single-threaded 1999 engine, rendered twice for stereo - not your GPU. If the
variance bothers you, running the headset at 72 Hz gives a rock-solid rate.

**Analog steering is experimental.** Steering is digital by default (thumbstick past a threshold acts
as an arrow key). The analog path exists behind a toggle but is unproven.

**Physics is framerate-coupled.** The engine advances its simulation with the frame rate, which is
why the community traditionally caps it at 24-30 fps. Higher rates work but change handling; if the
pod feels twitchy at high frame rates, that is why.

## Reporting problems

`hook.log` in the game folder, written fresh each launch. It opens with an environment block -
runtime, GPU, resolutions and every VR setting - which answers most questions on its own.

**Grab it before relaunching**, since it is overwritten. If the game crashed, include the timestamped
report from `crashes/` too; those carry a symbolised stack.

For anything obscure, turn on **Verbose logging** in the F5 panel (or set
`SWE1R_VR_VERBOSE=1`) to enable per-frame diagnostics - eye-pass divergence, framebuffer
status, input state, composite tracing.

**Environment variables may not reach the game.** This release of the game usually launches
*elevated*, and Windows builds an elevated process with a fresh environment, so a variable
you set in a shell or a shortcut is silently discarded. Both switches therefore also exist
as keys in the `[vr]` block of `SW_RACER_RE.ini`, which elevation cannot strip:

    [vr]
    no_vr=1      ; run flat, skip VR entirely
    verbose=1    ; per-frame diagnostics

Prefer the ini keys. The environment variables still work when they actually arrive.

## Troubleshooting

**Game will not start VR, runs flat.** Check the top of `hook.log`. `XR_ERROR_FORM_FACTOR_UNAVAILABLE`
means no headset was streaming when it launched - start Virtual Desktop first. No runtime at all
means you have no 32-bit OpenXR (see *Requirements*).

**Need to launch without VR:** set `no_vr=1` in the `[vr]` block of `SW_RACER_RE.ini`, or
`SWE1R_NO_VR=1` in the environment if it reaches the game (see above).

**Quit through the game's own menu.** Killing the process with a VR runtime loaded can leave it
unkillable until a reboot - that is a runtime-level behaviour, not specific to this mod.

## Credits

Built on [SW_RACER_RE](https://github.com/tim-tim707/SW_RACER_RE) by tim-tim707 and contributors,
whose decompilation and OpenGL renderer replacement made all of this possible. The original game is
the property of LucasArts / Lucasfilm.

`openxr_loader.dll` is the Khronos Group's OpenXR loader, redistributed unmodified under the
Apache License 2.0.

## Licence and source

This mod is licensed under the **GNU Affero General Public License, version 3**, inheriting the
licence of the SW_RACER_RE project it is built on. The full text is in `LICENSE`.

Complete corresponding source, including the VR layer, is published at:

    https://github.com/GameOrDie007/Star-Wars-Episode-I-Racer-PCVR

No game data is included in this download. You need your own legally obtained copy of
Star Wars Episode I: Racer.
