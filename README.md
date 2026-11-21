# Star Wars Episode I Racer PCVR

Star Wars Episode I: Racer (the 1999 PC release) in your PC VR headset, built as a layer on top of the [SW_RACER_RE](https://github.com/tim-tim707/SW_RACER_RE) community improvement mod. Stereo with true per-eye projection, head tracking, a 2D panel in the world for the HUD and menus, and full Quest controller support, on a game with no source, through a hook DLL.

**Watch the video:** [Star Wars Episode I Racer in VR (I can look out of the cockpit)](https://youtu.be/1uRlI-jZroA)

## Installing

1. Download **`Star Wars Episode I Racer PCVR Installer.exe`** from the
   [latest release](https://github.com/GameOrDie007/Star-Wars-Episode-I-Racer-PC/releases/latest)
   and double-click it. If Windows says it protected your PC, click **More info**, then
   **Run anyway**. The installer is not code-signed.
2. Press **INSTALL**. It finds your copy of the game (Steam, GOG or any folder), puts the mod in,
   checks your PC for anything that would stop VR from starting, and adds a desktop shortcut.
3. Start your headset streaming (Virtual Desktop or SteamVR), then press **PRESS START**.

Once it is installed, the game plays in VR however you start it: the desktop shortcut, Steam, or
`SWEP1RCR.EXE` itself.

**Updating keeps your settings.** Run the installer again and press **UPDATE**. Your saves and
`SW_RACER_RE.ini` are never touched.

### Installing by hand

Download the `.zip` from the same release, unzip it anywhere, and double-click **`Setup.bat`**. It
does what the installer does, in a console window. If it cannot find the game, drag the game
folder (the one holding `SWEP1RCR.EXE`) onto `Setup.bat`.

Setup puts three things next to `SWEP1RCR.EXE`, because the game only loads the mod from its own
folder: `dinput.dll` (the mod), `openxr_loader.dll` (the 32-bit OpenXR loader) and `assets\`
(shaders and HD fonts). Its own files go in a `VR` folder inside the game folder:

- `Star Wars Episode I Racer VR.cmd` starts the game in VR, and
  `Star Wars Episode I Racer (flat).cmd` starts it on the monitor without VR.
- `Uninstall.bat` removes everything Setup added and puts back any file it replaced (another
  mod's `dinput.dll`, for example). Your saves and settings stay.
- `Collect logs.bat` puts the logs and a short description of your PC in one zip on your desktop,
  for a bug report.

Nothing else is changed. If you would rather copy the files in yourself, take `dinput.dll`,
`openxr_loader.dll` and `assets\` from the zip's `files` folder and put them next to
`SWEP1RCR.EXE`; delete those three to remove it.

**Nothing else needs installing.** `dinput.dll` imports only Windows system libraries: no Visual
C++ redistributable, no other runtime.

**A stock Steam install of this game crashes on launch, and this mod fixes it.** You do not need
the `dinput.dll` that circulates on the Steam forums: ours sits in the same place and does the same
job.

**Custom tracks are not included.** The community mod's `assets\custom_tracks\` folder (user-made
courses, about 217 MB) has nothing to do with VR and is left out to keep the download small. To
add them, take that folder from the
[SW_RACER_RE release](https://github.com/tim-tim707/SW_RACER_RE/releases) and put it in the game's
`assets` folder.

`openvr_api.dll` is not needed. If you have one from an early build of this mod, delete it.

## Requirements

The game is a 32-bit program, so it needs a **32-bit OpenXR runtime**:

| Runtime | Works? |
|---|---|
| Virtual Desktop (VDXR) | Yes, it installs a 32-bit runtime |
| SteamVR | Yes, from 2.17 on (earlier versions were 64-bit only). Settings > OpenXR > *Set SteamVR as OpenXR Runtime*. Confirmed on a Rift CV1 and a Quest. This is the route for any tethered headset (Rift, Index, Vive, WMR), and the fix when Meta's runtime won't start |
| Meta / Oculus PC runtime | No. It registers a 32-bit entry, but creating a session fails with `XR_ERROR_RUNTIME_UNAVAILABLE`. Switch to SteamVR or Virtual Desktop |

Without a 32-bit runtime the game runs flat on the monitor, and `hook.log` says why. To check yours: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` must exist and point at a 32-bit DLL.

You also need a legitimate copy of the game (GOG or Steam; this was developed on Steam). The SW_RACER_RE mod is built in.

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
| X | Look back |
| Y | Repair |
| A | Boost, and confirm in menus |
| B | Back / pause |
| Left menu button | Pause / back |
| Both grips together | Recenter the 2D panel |

Either stick moves through menus, and the keyboard works alongside the controllers. B and the left menu button both back out of menus and both pause a race.

To boost, hold the left stick forward to charge and tap A. The boost holds as long as you keep the throttle on; you don't need to keep A held.

### Steering wheels (experimental)

A wheel is found automatically: the mod asks Windows what each device is, so a wheel turns wheel support on and a gamepad doesn't. Everything below can be remapped under **Input** in the F5 panel.

| Wheel | Action |
|---|---|
| Rim | Steering, fully analog |
| Pedals | Throttle and brake |
| D-pad | Menus |
| Left / right paddle | Roll left / right |
| A | Boost, and confirm in menus |
| B | Back / pause |
| LB | Charge boost (hold) |
| RB | Boost |
| X / Y | Look back / repair |
| + / - | Camera / slide |

The defaults are measured on a Logitech G923. Other wheels report different numbers, so use **Input > Wheel**: turn or press a control and the panel shows which axis or button moved.

Calibrate before racing: hold the wheel straight, press *Recalibrate wheel and pedals*, then turn lock to lock and press each pedal all the way. Wherever the wheel sits when you press it becomes center.

Two settings in the wheel's own software matter more than anything in the mod. In Logitech G HUB, turn on *Centering Spring in Non Force Feedback Games* (this game never drives force feedback, so without it the wheel goes slack), and lower *Operating Range* from 900 degrees to around 240 for much finer control.

## Camera views

The game's own camera button cycles five views: **Chase** (the default, behind the pod), **Cockpit** (in the pilot's seat), **Engine** (the original first-person view, between the engines), **Bumper** (low and in front) and **Far chase**, then back to Chase.

Cockpit view is where VR pays off. Your head sits where the pilot's is, with the cables and cowling around you and the engines pulling ahead. Each of the twenty-three pods has its own seat position, set in a headset one racer at a time, because they really differ (by 1.9 game units up and down and 4.7 front to back).

If a seat isn't quite right for you, **F5 > VR** names the racer you're flying and offers *Seat up / back / right*. The change is saved for that pod alone, and *Reset to built-in* puts it back. **Follow pod roll** sets how much the camera banks with the pod: 1 locks you to it, 0 keeps the horizon level, and lower is calmer if the rolling bothers you.

## Settings

**F5** opens the overlay. VR settings are under **VR** and are saved in a `[vr]` block in `SW_RACER_RE.ini`.

- **World units per metre**: how large the world feels, and the stereo separation with it. Raise it to shrink the world, lower it to enlarge it. Treat it as a comfort dial and set it to whatever makes the pods look right to you; `hook.log` reports a measured reference on its `[scale]` line.
- **Panel distance / width**: where the 2D panel sits. It stays put in the world, so you can look around it. **Recenter panel**, or squeezing both grips, brings it back in front of you. Your headset's own recenter (holding the Meta button) won't move a badly placed panel, because the panel moves with the whole tracking space; use the grips.
- **HUD size**: how much of your view the panel fills. 1.0 is the panel at its full width; above that it reaches into your peripheral vision, below it pulls the corners in. The HUD and the menus share the panel, so this changes both.
- **Overlay text size**: enlarges the F5 panel's own text.
- **Engine cull FOV boost**: the engine throws away scenery outside a cone built from the pod's camera. That cone now follows your head, so the ground no longer vanishes when you look down. Raise this only if you want it wider still (it costs frames); 1.0 hands culling back to the engine.
- **Nose-bob smoothing** (cockpit view, on): on rough stretches the pod's nose pitches several times a second. This steadies only that fast bobbing; steering, hills, ramps and jumps still follow the pod. 0 turns it off, and a higher *Nose-bob time* steadies slower bobbing too.
- **Bump smoothing** (cockpit view, off): filters track seams out of your view vertically. It works by holding your head still while the pod moves, so high values can put your viewpoint inside the cockpit's geometry. Worth trying if a track jolts you.
- **Menu: one stick direction at a time** (on): a stick pushed near a corner counts as one direction until it returns to center, so one flick no longer moves a list and changes a setting together. Menus only. Raise *Lock engages at tilt* if a drifting stick locks an axis by itself.
- **Controller haptics on impact** (on): the controllers buzz when your pod is hit, scaled by how hard. *Haptic strength* and *Haptic impact scale* tune it. The game's own **Force Feedback** screen still says *NO FORCE FEEDBACK DEVICE DETECTED*: it only looks for wheels and joysticks with motors, so it says the same on an unmodded install.
- **Suppress the game's screen-space flares** (on) and **Draw flares in world space (VR)** (on): the original flares can't stay put in VR, so they're redrawn as billboards that sit on their lamp posts as you turn your head. **Flare size (deg)** sets how wide they look; distant lights keep their size instead of shrinking away. The moon is about 0.5 degrees across, for reference.
- **Display scaling**: Windows display scaling (125%, 150%, normal on 4K monitors and laptops) no longer decides your VR resolution. Rendering at the full scaled resolution cost frames for no visible gain: on the development machine, misses went from 7.3% of frames to 1.0%. It does lower the headset resolution a little on a scaled display, and makes the monitor's mirror slightly softer. To keep the full resolution instead, set `dpi_unaware=0` in the `[vr]` block.

For performance, in **Render > Graphics Settings**: turn on *Cull off-screen meshes*, turn off *AI full LOD*, and leave the frame cap unlimited.

## Known limitations

- **Overhead place numbers are hidden.** The game stamps them on the flat HUD, so in VR they'd drift off the racers as you turn your head. Drawing them inside each eye would mean changing the working HUD path, so it's left alone.
- **Lens flares are stricter than the flat game.** They're real geometry in each eye, fogged and hidden behind terrain. The original's occlusion test is broken at 24/32 bpp and lets flares shine through walls, so you may see a few fewer.
- **Frame rate is in the mid-to-high 80s on a 90 Hz headset** on strong hardware. The limit is a single-threaded 1999 engine drawing everything twice, not your GPU. At 72 Hz it holds steady.
- **Some wheel buttons can't be reached.** On a G923, Start, Back and the clutch aren't visible to DirectInput at all. Bind a keystroke to them in the wheel's own software (Escape on Start gives you pause).
- **Wheel support has been tested on one wheel.** Every axis and button is a setting, so it should adapt, but expect to set the numbers yourself.
- **Physics follows the frame rate.** That's why the community traditionally caps it at 24 to 30 fps. Higher rates work but change the handling; if the pod feels twitchy, that's why.

## Reporting problems

Send `hook.log` from the game folder. It's written fresh each launch and opens with the runtime, GPU, resolutions and every VR setting, which answers most questions. Grab it before relaunching. If the game crashed, include the newest report from `crashes/` too.

For anything obscure, turn on **Verbose logging** in the F5 panel for per-frame diagnostics.

The game usually launches elevated, and Windows gives an elevated program a fresh environment, so environment variables you set may never reach it. Both switches are also keys in the `[vr]` block of `SW_RACER_RE.ini`:

    [vr]
    no_vr=1      ; run flat, skip VR entirely
    verbose=1    ; per-frame diagnostics

## Troubleshooting

### VR won't start and the game runs flat

Open `vr_report.txt` next to `SWEP1RCR.EXE`. It's a short file, written every launch, with only the VR startup lines, and it names the cause.

| What the report says | What it means | Fix |
|---|---|---|
| `NONE REGISTERED` | No 32-bit OpenXR runtime on the PC | Install Virtual Desktop, or SteamVR 2.17+ and set Settings > OpenXR > *Set SteamVR as OpenXR Runtime* |
| `XR_ERROR_RUNTIME_UNAVAILABLE` | A runtime is registered but won't start; Meta's PC runtime does this | Switch to SteamVR or Virtual Desktop |
| `XR_ERROR_FILE_ACCESS_ERROR` | Usually a broken OpenXR API layer, not the runtime (ReShade's is the usual one). A bad layer fails every runtime the same way | The report lists your layers and flags any with a missing file. Turn it off with [OpenXR-API-Layers-GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI) (it has a Win32 and a Win64 tab) |
| `XR_ERROR_GRAPHICS_DEVICE_INVALID` | The game is on the integrated GPU while the headset uses the other one. A low frame rate flat is the same symptom | Windows Settings > System > Display > Graphics > Add desktop app > `SWEP1RCR.EXE` > Options > High performance > Save, then relaunch |
| `XR_ERROR_FORM_FACTOR_UNAVAILABLE` | No headset at launch | Connect and start streaming first, then launch the game |

Don't run Virtual Desktop and SteamVR at the same time: they fight over the headset.

Don't overwrite `assets/` with the current SW_RACER_RE release. This mod forked an earlier version, and the newer shaders expect things this renderer doesn't have. For the HD model system, take only `assets/gltf/` (it ships empty: it's the framework, not a set of models).

To launch without VR, set `no_vr=1` in the `[vr]` block of `SW_RACER_RE.ini`.

Quit through the game's own menu. Killing the process with a VR runtime loaded can leave it stuck until a reboot.

## Credits

Built on [SW_RACER_RE](https://github.com/tim-tim707/SW_RACER_RE) by tim-tim707 and contributors, whose decompilation and OpenGL renderer replacement made all of this possible. Their own README, covering the decompilation and how to build it, is kept unchanged in [UPSTREAM_README.md](UPSTREAM_README.md). The original game is the property of LucasArts / Lucasfilm.

`openxr_loader.dll` is the Khronos Group's OpenXR loader, redistributed unmodified under the Apache License 2.0.

## License and source

This mod is under the **GNU Affero General Public License, version 3**, the license of the SW_RACER_RE project it's built on. The full text is in `LICENSE`. The complete source, including the VR layer, is here: https://github.com/GameOrDie007/Star-Wars-Episode-I-Racer-PC

No game data is included. You need your own legally obtained copy of Star Wars Episode I: Racer.

---

**Get an email when the next port ships:** follow [Game Or Die on Patreon](https://www.patreon.com/cw/GameOrDie) for free. Ports are never paywalled.
