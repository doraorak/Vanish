# Video

https://github.com/user-attachments/assets/cf8e6938-5a9d-41a1-a600-24e61c895216

# Vanish

Smooth window close animations for macOS on Apple Silicon.

Vanish replaces the close animation for every window on the system. It runs **inside `WindowServer` itself**, injected through **[TweakInject](https://github.com/doraorak/TweakInject)** — so it works with any application, with no plugins, no injection into apps, and no cooperation from the window being closed.

---

## ✨ Features

- **Runs inside the compositor.** Vanish hooks WindowServer's own window-ordering, teardown and event-delivery routines. None of them are exported — they're located by walking the symbol table at load time and pointer-signed before they're ever called.

- **Nothing client-side.** Traffic-light hit testing happens on the raw event stream inside the server. Vanish never loads into applications and never links AppKit. An app cannot tell it's there.

- **Starts when *you* click, not when the app gets around to it.** An AppKit or Chromium window doesn't disappear when you hit the red button — the app spends about a quarter of a second fading it out before it ever asks the server to remove it. Waiting for that request means the animation begins after the close is visually over. Vanish watches the window's own alpha instead and takes over the moment it starts to drop.

- **Four close animations** — **Shrink**, **Squish**, **Fall** and **Swirl** — selected in System Settings.

- **Hardware-accelerated warp.** The closing window's surface is cloned inside the server and deformed through `CGXWindow::set_mesh_warp`. Each animation declares the smallest grid that renders it exactly: a shrink or a topple is affine and needs nothing beyond its four corners, while a swirl, where different parts of the window travel along different curves, asks for a denser one.

- **Locked to the display.** Frames are scheduled against an absolute deadline rather than chained off one another, keeping the animation in step with the panel — a full 120 Hz on ProMotion.

- **Knows when not to run.** Some windows already come with a close animation: double-click a file in Finder and the app transposes the window back into its icon. Vanish detects those and stays out of the way instead of animating over them. System surfaces — Dock, menu bar, Notification Centre, wallpaper, Finder's Get Info panel — pass through untouched.

- **Drag-safe by construction.** The clone is built when you *release* the button, not when you press it. A press that turns into a window drag never creates one, so there is no stale clone to be uncovered when the window moves.

- **Preferences pane**:
  - **Master Enable/Disable**: Instant toggling of animations.
  - **Animation Duration Slider**: Fine-tune duration from 0.05s up to 2.0s.
  - **Refresh Rate Slider**: Adjust cadence up to 120 Hz, or 0 to follow the display.
  - **Animation Picker**: Choose which close animation plays.
  - **Shadow Toggle**: Choose whether to render the window shadow during the animation.

---

## 🔍 How a close plays out

1. **Intent.** A mouse-down inside the red button's region is noted, but nothing happens yet. Moving off the button, or dragging while held, cancels it. Only the release commits.

2. **Clone.** On release, the window's surface is cloned and ordered beneath the original — after the release has been handed on, so the work never sits between your click and the app hearing about it. The warp path is primed at the same time, so the first animated frame doesn't have to set itself up.

3. **Takeover.** Vanish watches the original's alpha and starts the moment the app begins fading it out, hiding the original so it can't show through from behind.

4. **Animate.** The chosen warp is applied to the clone on a fixed frame cadence. Only geometry is animated.

5. **Cleanup.** The clone is hidden, ordered out and released. If the close never actually arrives, it's discarded on a timeout.

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
- **Animation**: Shrink, Squish, Fall or Swirl. Applies to the next window you close.
- **Duration**: Adjust close speed between 0.05s and 2.0s (default: 0.25s).
- **Refresh Rate**: Adjust animation update frequency up to 120 Hz (default: 120 Hz). Set it to 0 to follow the display's own refresh rate.
- **Window Shadow**: Enable or disable the drop shadow on the animated window.

Preferences are stored in:
```
/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist
```

---

## 🧭 Logging

Vanish writes to `/tmp/vanish_ws.log`. Verbosity is set at compile time via `VN_LOG_LEVEL`; anything above the configured level is compiled out entirely, leaving no call and no format string in the binary.

| level | covers |
|---|---|
| `ERROR` | unresolved symbols, a clone that failed to build |
| `INFO` | one line per close (default) |
| `DEBUG` | hit tests, clone lifecycle, ordering decisions |
| `TRACE` | every order operation and every animation frame |

```bash
clang ... -DVN_LOG_LEVEL=VN_LOG_LEVEL_TRACE
```

---

## 📄 License

Created by **Dora Orak**.
