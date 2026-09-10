# Video

https://github.com/user-attachments/assets/cf8e6938-5a9d-41a1-a600-24e61c895216

# Vanish

Smooth window close animations for macOS on Apple Silicon.

Vanish hooks directly into the macOS window server compositor (`SkyLight` / `WindowServer`) via **[TweakInject](https://github.com/doraorak/TweakInject)** to provide fluid, interactive window close animations whenever you click a window's red traffic light button or close a window.

---

## ✨ Features

- **WindowServer-Level Compositor Hooking**: Hooks the server's own routines — `CGXOrderWindowListSpaceSwitchOptions` (every window-ordering path converges there), `CGXWindow::release_window`, and `CGXPostEventByConnection` — so closing windows animate without app-specific plugins. None of these are exported; they are resolved by name at runtime from the symbol table.
- **Fully server-side**: Traffic-light hit testing is done on the event stream inside WindowServer. Vanish never talks to the closing application and relies on nothing client-side.
- **Starts on the fade, not the order-out**: AppKit and Chromium windows don't vanish when closed — the app fades its alpha to zero over ~250ms and only *then* orders the window out. Vanish watches the alpha and takes over the moment it starts dropping, roughly 250ms earlier than the order-out would allow.
- **Hardware-Accelerated Warp Mesh**: Clones the closing window's surface and drives a uniform shrink through `CGXWindow::set_mesh_warp`. The mesh is 2×2 — the shrink is affine, and bilinear interpolation of a quad's corners reproduces an affine map exactly, so four points carry the same image 25 did.
- **ProMotion-aware frame pacing**: Frames are scheduled on an absolute deadline rather than relative to the previous callback, so the server timer's ~0.8ms lateness cannot accumulate. Measured on a 120 Hz display: 8.30ms mean frame interval against an 8.33ms target, with 1.3% of frames arriving late.
- **Defers to system close animations**: Some windows already have one — double-click a file in Finder and the app transposes the window back into its icon on close. Vanish detects that and stays out of the way rather than animating on top of it.
- **Preferences Pane**:
  - **Master Enable/Disable**: Instant toggling of animations.
  - **Animation Duration Slider**: Fine-tune duration from 0.05s up to 1.0s.
  - **Refresh Rate Slider**: Adjust cadence from 10 Hz up to 120 Hz with live feedback.
  - **Shadow Toggle**: Choose whether to render the window shadow during the shrink animation.
- **No clone left behind**: The clone is only built once a press is confirmed by a release still on the button — a press that turns into a window drag never creates one, so dragging can never expose a stale clone. Anything that does get built is discarded on a timeout if the close never arrives.

---

## 🔍 How a close plays out

1. **Intent.** A mouse-down inside the red button's region is recorded, but nothing happens yet. A drag off the button, or more than 4pt of movement while held, cancels it. Only the release commits.
2. **Pre-clone.** On release, the window's surface is cloned and ordered below the original — after the release has been forwarded, so the clone build (2ms on a light window, 16ms on a heavy one) stays off the input path.
3. **Takeover.** Vanish watches the original's alpha and starts the moment the app begins fading it, hiding the original so it can't show through behind the clone.
4. **Shrink.** A uniform scale about the window's centre, on an absolute frame deadline. Despite the internal log wording, nothing fades — only geometry is animated.
5. **Cleanup.** The clone is hidden, ordered out and released; if the close never lands, a timeout discards it.

---

## 🛠 Prerequisites

- **macOS 15.0+** (tested up to macOS 27 / Sequoia+)
- **Apple Silicon** (`arm64e`)
- **System Integrity Protection (SIP) disabled** (required for `WindowServer` injection)
- **[tweakLoader](https://github.com/doraorak/tweakLoader) / [TweakInject](https://github.com/doraorak/TweakInject)** installed

---

## 📦 Building

To build the tweak bundle and generate a Debian package (`.deb`):

```bash
./package.sh
```

This compiles:
- `Vanish.bundle` targeting `arm64e` linked with `SkyLight`, `CoreGraphics`, and `ellekit`.
- `VanishPrefs.bundle` companion preference pane for System Settings.
- Outputs a ready-to-install `.deb` in `packages/`.

Alternatively, if you have a working Theos environment:
```bash
make package
```

---

## 🚀 Installation

### Option 1: TweakInject Store (Recommended)
Install directly from the **TweakInject Store** app.

### Option 2: Debian Package (`.deb`)
```bash
sudo dpkg -i packages/com.doraorak.vanish_*.deb
```

### Option 3: Manual Bundle Install
Copy `Vanish.bundle` directly into the TweakInject bundle directory:
```bash
sudo cp -R Vanish.bundle /Library/TweakInject/Tweaks/Bundles/
```
And copy preferences:
```bash
sudo cp -R layout/Library/TweakInject/Preferences/* /Library/TweakInject/Preferences/
```

---

## ⚙️ Configuration

Open **System Settings** → **TweakInject** → **Vanish** to configure:
- **Enable Animation**: Toggle window close animations on/off.
- **Duration**: Adjust close speed between 0.05s and 1.0s (default: 0.35s).
- **Refresh Rate**: Adjust animation update frequency between 10 Hz and 120 Hz (default: 120 Hz). Set to 0 to follow the display's actual refresh rate instead.
- **Window Shadow**: Enable or disable the drop shadow on the animated window.

Preferences are stored in:
```
/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist
```

---

## 🧭 Logging

Logging is compile-time levelled (`VN_LOG_LEVEL`, default `INFO`) and writes to `/tmp/vanish_ws.log`. Anything above the configured level compiles out entirely — no call, no format string in the binary.

| level | what it covers |
|---|---|
| `ERROR` | unresolved symbols, a clone that failed to build, safety backstops firing |
| `INFO` | one line per close: which path it took and when |
| `DEBUG` | per-gesture detail — hit tests, pre-clone lifecycle, ordering decisions |
| `TRACE` | firehose: every order operation, every animation frame's timing |

Rebuild at a different level with `-DVN_LOG_LEVEL=VN_LOG_LEVEL_TRACE`.

---

## 📄 License

Created by **Dora Orak**.
