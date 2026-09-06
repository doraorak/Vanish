#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

NAME="Vanish"
PKG_ID="com.doraorak.vanish"
BUNDLE="$NAME.bundle"

SDK_PATH=$(xcrun --show-sdk-path --sdk macosx)

# A .bundle rather than a bare dylib, which is what the loader's first scan pass
# looks for: it walks /Library/TweakInject/Tweaks/Bundles, takes the first file
# in Contents/MacOS as the executable and reads the filter from
# Contents/Resources/Filter.plist. See tl_find_bundle_executable() and
# tl_read_bundle_filter() in tweakLoader.c.
echo "==> 1. Compiling $NAME (arm64e)..."
rm -rf "$DIR/$BUNDLE"
mkdir -p "$DIR/$BUNDLE/Contents/MacOS" "$DIR/$BUNDLE/Contents/Resources"

# -x objective-c, not objective-c++: this is plain ObjC and compiling it as C++
# drags in libc++ for nothing -- inside the window server, of all places.
#
# SkyLight is linked for the SLS* symbols. It is a private framework, hence the
# explicit -F; the SDK does not put it on the default search path.
#
# ellekit provides MSHookFunction. Its install name is /usr/local/lib, which is
# where the loader keeps it, so -L only matters at link time.
clang -dynamiclib -arch arm64e -isysroot "$SDK_PATH" -fobjc-arc -x objective-c \
    -F"$SDK_PATH/System/Library/PrivateFrameworks" \
    -framework Foundation -framework CoreGraphics -framework SkyLight \
    -L/Library/TweakInject -lellekit \
    -install_name "/Library/TweakInject/Tweaks/Bundles/$BUNDLE/Contents/MacOS/$NAME" \
    -o "$DIR/$BUNDLE/Contents/MacOS/$NAME" "$DIR/$NAME.m"

PREFS_BUNDLE="${NAME}Prefs.bundle"
PREFS_SRC="$DIR/layout/Library/TweakInject/Preferences/PreferenceBundles/$PREFS_BUNDLE"
PKG_VERSION="$(awk '/^Version:/{print $2}' "$DIR/control")"

mkdir -p "$PREFS_SRC"
cat > "$PREFS_SRC/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>${PKG_ID}prefs</string>
	<key>CFBundleName</key>
	<string>$PREFS_BUNDLE</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>$PKG_VERSION</string>
</dict>
</plist>
PLIST

cp "$DIR/Filter.plist" "$DIR/$BUNDLE/Contents/Resources/Filter.plist"
if [ -f "$PREFS_SRC/Root.plist" ]; then
    cp "$PREFS_SRC/Root.plist" "$DIR/$BUNDLE/Contents/Resources/Root.plist"
fi
if [ -f "$DIR/assets/icon.svg" ]; then
    cp "$DIR/assets/icon.svg" "$DIR/$BUNDLE/Contents/Resources/icon.svg"
    cp "$DIR/assets/icon.svg" "$PREFS_SRC/icon.svg"
fi
# Embed companion preference bundle inside the tweak bundle
rm -rf "$DIR/$BUNDLE/Contents/Resources/$PREFS_BUNDLE"
cp -R "$PREFS_SRC" "$DIR/$BUNDLE/Contents/Resources/$PREFS_BUNDLE"

cat > "$DIR/$BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>$PKG_ID</string>
	<key>CFBundleName</key>
	<string>$NAME</string>
	<key>CFBundleExecutable</key>
	<string>$NAME</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>$(awk '/^Version:/{print $2}' "$DIR/control")</string>
</dict>
PLIST
echo "</plist>" >> "$DIR/$BUNDLE/Contents/Info.plist"

# Ad-hoc, like every other injected component here. Library validation is
# disabled system-wide, which is the only reason anything unsigned-by-Apple
# loads into WindowServer at all.
codesign -f -s - "$DIR/$BUNDLE"

echo "==> 2. Staging..."
STAGE_DIR="$DIR/.stage"
rm -rf "$STAGE_DIR"
if [ -d "$DIR/layout" ]; then
    cp -R "$DIR/layout/" "$STAGE_DIR/"
fi
mkdir -p "$STAGE_DIR/Library/TweakInject/Tweaks/Bundles"
cp -R "$DIR/$BUNDLE" "$STAGE_DIR/Library/TweakInject/Tweaks/Bundles/"

# Version comes from ./control so the two cannot drift.
PKG_VERSION="$(awk '/^Version:/{print $2}' "$DIR/control")"
OUTPUT_DEB="$DIR/packages/${PKG_ID}_${PKG_VERSION}_darwin-arm64e.deb"

echo "==> 3. Generating standard .deb package..."
python3 - "$STAGE_DIR" "$DIR/control" "$OUTPUT_DEB" << 'PYEOF'
import sys, os, tarfile, tempfile, shutil

stage_dir, control_file, output_deb = sys.argv[1], sys.argv[2], sys.argv[3]

def create_ar_header(filename, size):
    return (filename.ljust(16) + "0".ljust(12) + "0".ljust(6) + "0".ljust(6)
            + "100644".ljust(8) + str(size).ljust(10) + "`\n").encode('ascii')

temp_dir = tempfile.mkdtemp()
try:
    deb_bin = b"2.0\n"
    control_tar = os.path.join(temp_dir, "control.tar.gz")
    with tarfile.open(control_tar, "w:gz") as tar:
        tar.add(control_file, arcname="./control")
    with open(control_tar, "rb") as f:
        c_data = f.read()

    data_tar = os.path.join(temp_dir, "data.tar.gz")
    with tarfile.open(data_tar, "w:gz") as tar:
        for item in sorted(os.listdir(stage_dir)):
            tar.add(os.path.join(stage_dir, item), arcname=f"./{item}")
    with open(data_tar, "rb") as f:
        d_data = f.read()

    os.makedirs(os.path.dirname(os.path.abspath(output_deb)), exist_ok=True)
    with open(output_deb, "wb") as deb:
        deb.write(b"!<arch>\n")
        deb.write(create_ar_header("debian-binary", len(deb_bin)))
        deb.write(deb_bin + (b"\n" if len(deb_bin) % 2 else b""))
        deb.write(create_ar_header("control.tar.gz", len(c_data)))
        deb.write(c_data + (b"\n" if len(c_data) % 2 else b""))
        deb.write(create_ar_header("data.tar.gz", len(d_data)))
        deb.write(d_data + (b"\n" if len(d_data) % 2 else b""))
finally:
    shutil.rmtree(temp_dir)
print(f"Created: {output_deb}")
PYEOF

rm -rf "$STAGE_DIR"
echo "==> 4. Exporting to packages/..."
rm -rf "$DIR/packages/$BUNDLE"
cp -R "$DIR/$BUNDLE" "$DIR/packages/$BUNDLE"
if [ -d "$DIR/layout/Library/TweakInject/Preferences/PreferenceBundles/VanishPrefs.bundle" ]; then
    rm -rf "$DIR/packages/VanishPrefs.bundle"
    cp -R "$DIR/layout/Library/TweakInject/Preferences/PreferenceBundles/VanishPrefs.bundle" "$DIR/packages/VanishPrefs.bundle"
fi

if [ -f "$DIR/tools/VanishTest.m" ]; then
    echo "==> 5. Compiling VanishTest test app..."
    clang -arch arm64e -isysroot "$SDK_PATH" -framework Cocoa -o "$DIR/packages/VanishTest" "$DIR/tools/VanishTest.m"
    codesign -f -s - "$DIR/packages/VanishTest"
fi

echo "==> Done: $OUTPUT_DEB, $DIR/packages/$BUNDLE"

