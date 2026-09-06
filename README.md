# Video

https://github.com/user-attachments/assets/cf8e6938-5a9d-41a1-a600-24e61c895216

# Vanish

Smooth window close animations for macOS on Apple Silicon.

Vanish hooks directly into the macOS window server compositor (`SkyLight` / `WindowServer`) via **[TweakInject](https://github.com/doraorak/TweakInject)** to provide fluid, interactive window close animations whenever you click a window's red traffic light button or close a window.

---

## ✨ Features

- **WindowServer-Level Compositor Hooking**: Hooks `SkyLight` window management routines (`SLSOrderWindow`, `_XOrderWindow`, and AppKit traffic light tracking) to seamlessly animate closing windows without relying on app-specific plugins.
- **Hardware-Accelerated Warp Mesh**: Clones the closing window backing store and drives smooth shrink & fade mesh warps directly through SkyLight surface transforms.
- **ProMotion 120 Hz Display Synchronization**: Tight display refresh loop capable of locking scanout to full 120 Hz on ProMotion displays with zero dropped frames.
- **Preferences Pane**:
  - **Master Enable/Disable**: Instant toggling of animations.
  - **Animation Duration Slider**: Fine-tune duration from 0.05s up to 1.0s.
  - **Refresh Rate Slider**: Adjust cadence from 10 Hz up to 120 Hz with live feedback.
  - **Shadow Toggle**: Choose whether to render the window shadow during the shrink animation.
- **Zero Glitch Guarantee**: Clean pre-close clone handoff, boundary clipping, and instant cleanup upon completion to prevent flashes or ghost rectangles.

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
- **Refresh Rate**: Adjust animation update frequency between 10 Hz and 120 Hz (default: 120 Hz).
- **Window Shadow**: Enable or disable the drop shadow on the animated window.

Preferences are stored in:
```
/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist
```

---

## 📄 License

Created by **Dora Orak**.
