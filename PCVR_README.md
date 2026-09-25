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
| SteamVR | **Yes, from 2.17 onward** - earlier versions were 64-bit only. Settings > OpenXR > *Set SteamVR as OpenXR Runtime*. Confirmed by two users independently, on a Rift CV1 and on a Quest. This is the route for any tethered headset (Rift, Index, Vive, WMR), and the fix if Meta's runtime will not start |
| Meta / Oculus PC runtime | **No.** It registers a 32-bit entry, so it looks like it should work, but creating a session fails with `XR_ERROR_RUNTIME_UNAVAILABLE`. Reported on a Quest, and switching that machine to SteamVR fixed it immediately. If you are on Meta's runtime, switch - do not spend time on it |

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
| Right stick click | Slide |
| X | **Look back** |
| Y | Repair |
| A | Boost, and confirm in menus |
| B | **Back** / pause |
| Left menu | Pause / **Back** |
| **Both grips together** | **Recentre the 2D panel** |

### Steering wheels (experimental)

A wheel is detected automatically and needs no setup: the mod asks Windows what each
attached device is, so a wheel switches wheel support on and a gamepad does not. Everything
below is remappable under **Input** in the F5 panel.

| Wheel | Action |
|---|---|
| Rim | Steering, fully analog |
| Pedals | Throttle and brake |
| D-pad | Menu navigation |
| Left / right paddle | Roll left / right |
| A | Boost, and confirm in menus |
| B | Back / pause |
| LB | Charge boost (hold) |
| RB | Boost |
| X / Y | Look back / repair |
| + / - | Camera / slide |

The defaults are a Logitech G923 map, measured on one. Another wheel reports different
numbers, so use **Input > Wheel** to set them: turn or press a control and the panel lists
whichever axis or button moved.

**Calibrate before racing.** Hold the wheel straight, press *Recalibrate wheel and pedals*,
then turn lock to lock and floor each pedal. The moment you press it, wherever the wheel is
sitting becomes centre, so it only matters that it is straight then.

**Two settings on the wheel itself are worth more than anything in the mod.** In Logitech G
HUB, turn on *Centering Spring in Non Force Feedback Games* (this game never drives force
feedback, so without it the wheel goes slack), and drop *Operating Range* from 900 degrees to
around 240 - a podracer wants a quick input, and a shorter range gives far finer control.

**Either thumbstick navigates menus**, in both directions. Keyboard continues to work
alongside the controllers.

**There are two Back buttons**, and they work the same everywhere: B and the left menu
button both back out of menus, and both pause a race. No mode switching, nothing to learn.

**Slide is on the right thumbstick click**, and **Look back is on X**. Freeing B is what
let it become an unambiguous Back.

X was available because it never did anything: two controller actions had been given the
same display name, OpenXR rejected the second, and the button had been silently dead since
the first release. It is a real button again.

**Boost is A.** Hold the left stick forward to charge, tap A to fire it, and it sustains
itself as long as you hold the throttle - you do not need to keep A held.

## Camera views

The game's own camera button cycles five views, in this order:

| View | |
|---|---|
| **Chase** | the default, behind the pod |
| **Cockpit** | sitting in the pod, as the pilot |
| **Engine** | retail's first-person view, out between the engines |
| **Bumper** | low and in front |
| **Far chase** | pulled well back |

Then back to Chase. It is the same button as the flat game - nothing new to learn, and no menu
to go into.

**Cockpit view is where VR pays off.** It puts your head where the pilot's is, so the pod's
cables and cowling sit around you and the engines pull from ahead. Every one of the
twenty-three pods has its own seat position, measured in a headset one racer at a time,
because they genuinely differ - the tuned values span 1.9 game units vertically and 4.7
front-to-back, so no single offset could sit correctly in all of them.

If a seat is not quite right for you, **F5 > VR** names the racer you are flying and gives you
*Seat up / back / right*. Your change is saved against that pod alone, and *Reset to built-in*
puts it back. **Follow pod roll** decides how much the camera banks with the pod: 1 welds you
to it, 0 keeps the horizon level, and lower is calmer if the rolling is uncomfortable.

## Settings

**F5** opens the debug overlay. VR settings live under the **VR** section and are saved to a `[vr]`
block in `SW_RACER_RE.ini`, so they persist between sessions.

Worth tuning:

- **World units per metre** - how large the world feels. Affects stereo separation. Raise it to
  shrink the world, lower it to enlarge.

  Earlier versions of this page claimed the default was derived from the pod's dimensions, on
  the basis that a podracer is about 7 m. That is wrong: 7 m is one *engine*, not the whole
  vehicle, so the reference was comparing the wrong part and the figure it produced does not
  mean what it said. The panel now measures a citable part instead, and `hook.log` reports
  what it found on a `[scale]` line. The default is unchanged pending enough real readings to
  set it honestly - so treat this as a comfort dial, not a calibrated one, and set it to
  whatever makes the pods look right to you.
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
- **Engine cull FOV boost** - a floor, not the main control. The engine culls to a cone built
  from the pod camera and knows nothing about your head, so scenery you look at by turning can
  already have been thrown away. That cone now follows your head automatically, which is what
  fixed ground disappearing below a straight line when you looked down from a seated position.
  Raise this only if you want it wider still; 1.0 turns both off and hands culling back to the
  engine. Costs performance, so lower it if you need frames.
- **Bump smoothing** (cockpit view) - **off by default.** The cockpit camera is bolted rigidly
  to the pod, so a seam in the track goes straight into your head. This filters that out
  vertically - but because it works by holding your head still while the pod moves under it, at
  higher values it can move your viewpoint into the pod's own cockpit geometry. Worth trying if
  a track jolts you; 0 is the camera that shipped in v1.4.
- **Menu: one stick direction at a time** - on by default. A thumbstick near a corner counts as
  two directions at once, so one flick used to move a list *and* change a setting. The first
  direction past the tilt threshold now claims the stick until it returns to centre. Menus
  only - racing is untouched. Raise *Lock engages at tilt* if a drifting stick locks an axis on
  its own.
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

- **Windows display scaling no longer decides your VR resolution.** If your desktop runs at
  125%, 150% or any other scaling - normal on 4K monitors and most laptops - earlier versions
  rendered at your monitor's full physical resolution and handed that to the headset. On a 4K
  screen at 150% that is 8.3 megapixels an eye instead of 3.7, for no visible gain, and it was
  usually the difference between holding 90 fps and not.

  Measured on the development machine, same race, twenty minutes apart: v1.3 finished each
  frame with 0.25 ms to spare and missed 7.3% of them; v1.4 has 2.89 ms to spare and misses
  1.0%. Nothing to configure - it just stops asking the desktop.

  Two side effects worth knowing, one of which the v1.4 notes got wrong. On a scaled display
  the desktop mirror window is now scaled up by Windows, so the picture on your monitor is
  slightly softer than before. And the headset image is **not** unaffected, as v1.4 claimed:
  the eye texture is copied from the game's framebuffer, so on a scaled display this lowers
  headset resolution too. That is the trade - frames for pixels - and on the machines where it
  was measured it was plainly worth it, but it is a trade and it should have been described as
  one.

  If you would rather have the pixels, set `dpi_unaware=0` in the `[vr]` block of
  `SW_RACER_RE.ini` and you get the v1.3 behaviour back.

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

**Some wheel buttons cannot be reached.** On a G923, Start, Back and the clutch pedal are not
exposed to DirectInput at all - not by the game, and not by reading the device directly - so
nothing here can bind them. If you want them, bind a keystroke to them in your wheel's own
software: Escape on Start gives you pause.

**Wheel support is new and has been tested on exactly one wheel.** It should adapt to others,
since every axis and button is a setting, but expect to set the numbers yourself.

**Steering is fully analog.** Small stick or wheel movements give small turns. Earlier
releases thresholded the stick into arrow-key presses, which felt like a dead zone
followed by full lock; that is fixed.

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

### VR will not start - the game runs flat on the monitor

**Open `vr_report.txt`** in the game folder, next to `SWEP1RCR.EXE`. It is a short file
written fresh every launch containing only the VR startup lines, and it names the cause. (The
same lines are in `hook.log`, about 2,500 lines down, which is why the short file exists.)

Every failure below has been seen by a real player. Match the error and skip to the fix.

| What the report says | What it means | Fix |
|---|---|---|
| `NONE REGISTERED` | No 32-bit OpenXR runtime on the PC. The game is 32-bit, so a 64-bit-only runtime cannot serve it however well the headset works elsewhere | Install Virtual Desktop, or SteamVR 2.17+ and set Settings > OpenXR > *Set SteamVR as OpenXR Runtime* |
| `XR_ERROR_RUNTIME_UNAVAILABLE` | A runtime is registered but would not start. Meta's PC runtime does this - it registers a 32-bit entry that does not work | Switch to SteamVR or Virtual Desktop. Do not spend time on Meta's runtime |
| `XR_ERROR_FILE_ACCESS_ERROR` | Something the loader needed could not be read - usually a broken **OpenXR API layer**, not the runtime. ReShade's is the usual culprit. A bad layer fails *every* runtime identically, so swapping runtimes will not help | The report lists your layers and flags any whose file is missing. Uncheck it - [OpenXR-API-Layers-GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI) lists them, with a Win32 and a Win64 tab |
| `XR_ERROR_GRAPHICS_DEVICE_INVALID` | The runtime rejected the **graphics card** the game is using. Almost always the game is on the integrated GPU while the headset runs off the discrete one. A low frame rate in flat mode is the same symptom | Windows Settings > System > Display > **Graphics** > Add desktop app > `SWEP1RCR.EXE` > Options > **High performance** > Save, then relaunch |
| `XR_ERROR_FORM_FACTOR_UNAVAILABLE` | No headset was available at launch | Connect and start streaming **first**, then launch the game |

**Do not run Virtual Desktop and SteamVR at the same time.** They compete for the headset and
neither gets a clean session. Use one or the other.

**Do not overwrite `assets/` with the upstream SW_RACER_RE release.** This mod forked that
project some time ago and upstream has moved a long way since; its shaders expect things this
renderer does not provide, and `assets/shaders/` is required at runtime. If you want the HD
model system, take `assets/gltf/` only - and note it ships empty, because it is the
replacement framework, not a set of models.

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
