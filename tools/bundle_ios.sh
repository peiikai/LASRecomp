#!/bin/bash
# make ipa: build/ios/LAS Recomp.app and build/dist/LAS Recomp-<version>-ios.ipa from build/ios/lasrecomp and
# build/ios/lasgame.dylib. The app is laid out as on macOS: the executable holds no game code; the game code library
# (built from your own game files) is the embedded Frameworks/LASGame.framework, signed with the app - iOS runs only
# code signed inside the bundle, so the .ipa holds the game's code: for your own devices, never shared.
# The data folder on the device is the app's Documents (Files app / Finder): put your extracted/ (exefs + romfs)
# there; saves, seeds and lasrecomp.log live there too. Fonts and the icon ship in the bundle.
#
#   TEAM_ID=<id>          the Apple Developer team (the entitlement in runtime/platform/ios.entitlements: any team)
#   PROFILE=<.mobileprovision>  a development profile for BUNDLE_ID with those capabilities and this device
#   BUNDLE_ID=...         default com.peiikai.lasrecomp.ios
#   SIGN="Apple Development: ..."  the identity (default: the profile's certificate that is in the keychain)
#   DEVICE=<devicectl id>  install and launch on that device
#   SKELETON=1            no game library and no signing: the app without Frameworks/LASGame.framework, which the
#                         iOS skeleton zip ships (tools/ios_skeleton.sh) and the start screen on any desktop
#                         completes with the library it builds there (runtime/iosexport.c); the .ipa from that is
#                         for the player's own sideloading tool: the executable carries an ad-hoc signature with
#                         runtime/platform/ios.entitlements (one entitlement, which a free Apple ID's team grants),
#                         where such tools read what to ask Apple for. Symlinks in the app become <name>.symlink
#                         files holding the target (the zip is unpacked on Windows too)
set -euo pipefail
cd "$(dirname "$0")/.."
EXE=${1:-build/ios/lasrecomp}
LIB=${2:-$(dirname "$EXE")/lasgame.dylib}
SKELETON=${SKELETON:-}
APP=${APP:-build/ios/LAS Recomp.app}
BUNDLE_ID=${BUNDLE_ID:-com.peiikai.lasrecomp.ios}
IOS_MIN=${IOS_MIN:-16.0}
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
case "$VERSION" in
    beta_*) SHORT="0.9.${VERSION#beta_}" ;;
    v*) SHORT="${VERSION#v}" ;;
    *) SHORT="0.0.1" ;;
esac
[ -x "$EXE" ] || { echo "no $EXE (make PLATFORM=ios)"; exit 1; }
if [ -z "$SKELETON" ]; then
    [ -f "$LIB" ] && [ -f "$LIB.id" ] || { echo "no $LIB (make PLATFORM=ios gen, then make PLATFORM=ios)"; exit 1; }
fi
# the app must hold no game code (grep without -q: with pipefail, nm cut off early would hide a match)
if nm "$EXE" | grep ' _f_8' >/dev/null; then echo "$EXE has game code linked in"; exit 1; fi

rm -rf "$APP"
mkdir -p "$APP/Frameworks"
cp "$EXE" "$APP/lasrecomp"
strip -S "$APP/lasrecomp"

# The game code: Frameworks/LASGame.framework (a flat iOS framework: the binary, Info.plist, its .id next to it).
# The skeleton gets only its Info.plist (runtime/iosexport.c adds the library and its .id).
FW="$APP/Frameworks/LASGame.framework"
mkdir -p "$FW"
if [ -z "$SKELETON" ]; then
    cp "$LIB" "$FW/LASGame"
    strip -S -x "$FW/LASGame"
    cp "$LIB.id" "$FW/LASGame.id"
fi
cat > "$FW/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>LASGame</string>
	<key>CFBundleIdentifier</key><string>$BUNDLE_ID.game</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>LASGame</string>
	<key>CFBundlePackageType</key><string>FMWK</string>
	<key>CFBundleShortVersionString</key><string>$SHORT</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
	<key>MinimumOSVersion</key><string>$IOS_MIN</string>
</dict>
</plist>
EOF
cp docs/icon/icon_ios_1024.png "$APP/icon.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$APP/"
cp third_party/fonts/Baloo2/OFL.txt "$APP/Baloo2-OFL.txt"
# the game text's fonts (runtime/hle/pl.c: no readable system fonts on iOS, so BIZ UDPGothic serves first) and the
# last-resort ones, as the desktop packages ship them
cp third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf "$APP/"
cp third_party/fonts/BIZUDPGothic/OFL.txt "$APP/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$APP/"
cp third_party/fonts/DejaVu/LICENSE "$APP/DejaVu-LICENSE.txt"

# App icon: the asset catalog compiled from the 1024 png (Assets.car + CFBundleIcons); skipped without actool.
if command -v xcrun >/dev/null && xcrun --find actool >/dev/null 2>&1; then
    # (docs/icon/icon_ios_1024.png: the art full-bleed and opaque - iOS rounds the corners itself; from icon_ios.svg)
    ICONSET=build/ios/AppIcon.xcassets/AppIcon.appiconset
    rm -rf build/ios/AppIcon.xcassets && mkdir -p "$ICONSET"
    cp docs/icon/icon_ios_1024.png "$ICONSET/icon.png"
    cat > "$ICONSET/Contents.json" <<'EOF'
{ "images": [ { "filename": "icon.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" } ],
  "info": { "author": "xcode", "version": 1 } }
EOF
    echo '{ "info": { "author": "xcode", "version": 1 } }' > build/ios/AppIcon.xcassets/Contents.json
    xcrun actool build/ios/AppIcon.xcassets --compile "$APP" --platform iphoneos --minimum-deployment-target "$IOS_MIN" \
        --app-icon AppIcon --output-partial-info-plist build/ios/icon-plist.plist >/dev/null
fi

cat > "$APP/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleDisplayName</key><string>LAS Recomp</string>
	<key>CFBundleExecutable</key><string>lasrecomp</string>
	<key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>LAS Recomp</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$SHORT</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
	<key>DTPlatformName</key><string>iphoneos</string>
	<key>DTSDKName</key><string>iphoneos</string>
	<key>LSRequiresIPhoneOS</key><true/>
	<key>MinimumOSVersion</key><string>$IOS_MIN</string>
	<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
	<key>UIFileSharingEnabled</key><true/>
	<key>LSSupportsOpeningDocumentsInPlace</key><true/>
	<key>UILaunchScreen</key><dict/>
	<key>UIRequiresFullScreen</key><true/>
	<key>UIStatusBarHidden</key><true/>
	<key>UIViewControllerBasedStatusBarAppearance</key><false/>
	<key>UIRequiredDeviceCapabilities</key><array><string>arm64</string><string>metal</string></array>
	<key>UISupportedInterfaceOrientations</key>
	<array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
	<key>UISupportedInterfaceOrientations~ipad</key>
	<array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
	<key>GCSupportsControllerUserInteraction</key><true/>
	<key>UIApplicationSupportsIndirectInputEvents</key><true/>
	<key>ITSAppUsesNonExemptEncryption</key><false/>
EOF
cat >> "$APP/Info.plist" <<'EOF'
</dict>
</plist>
EOF
# the icon catalog's keys (CFBundleIcons, CFBundleIcons~ipad), when compiled
if [ -f build/ios/icon-plist.plist ]; then
    python3 - "$APP/Info.plist" build/ios/icon-plist.plist <<'EOF'
import plistlib, sys
with open(sys.argv[1], 'rb') as f: info = plistlib.load(f)
with open(sys.argv[2], 'rb') as f: info.update(plistlib.load(f))
with open(sys.argv[1], 'wb') as f: plistlib.dump(info, f)
EOF
fi
plutil -lint "$APP/Info.plist" >/dev/null

# ---- the embedded CPython (runtime/platform/ios_python.m): Python.framework, the stdlib under python/lib, and every
# C extension (.so: stdlib's and site-packages') as its own Frameworks/<module>.framework, with a <module>.fwork file
# left in its place for the importer (CPython's iOS loader) - the layout of BeeWare's testbed, build/utils.sh.
DEPS=${DEPS:-build/deps-ios}
PYV=${PYV:-3.13}  # (the Makefile's)
PYSLICE=$DEPS/Python.xcframework/ios-arm64
if [ -f "$PYSLICE/Python.framework/Python" ]; then
    cp -R "$PYSLICE/Python.framework" "$APP/Frameworks/Python.framework"
    mkdir -p "$APP/python/lib"
    rsync -a "$DEPS/Python.xcframework/lib/" "$APP/python/lib/"  # (the pure-Python stdlib, shared by the slices)
    rsync -a --exclude 'libpython*.dylib' "$PYSLICE/lib-arm64/" "$APP/python/lib/"  # (this slice's extensions)
    # (no tests, no idle / tkinter: size)
    rm -rf "$APP"/python/lib/python$PYV/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,pydoc_data,__pycache__}
    find "$APP/python/lib" -name "__pycache__" -type d -prune -exec rm -rf {} +
    find "$APP/python/lib" -name "*_test*.so" -o -name "xx*.so" | xargs rm -f  # (the test extensions)
    [ -d "$DEPS/python/site-packages" ] && cp -R "$DEPS/python/site-packages" "$APP/python/site-packages"
    # the generator's sources: tools/rando as it is (less the desktop helper's venv / spec and caches), the randomizer's
    # core and resources
    mkdir -p "$APP/rando-gen"
    rsync -a --exclude '__pycache__' --exclude 'venv' --exclude '*.spec' --exclude '.*' tools/rando/ "$APP/rando-gen/"
    rsync -a --exclude '__pycache__' third_party/LAS-Randomizer/RandomizerCore third_party/LAS-Randomizer/Resources "$APP/rando-gen/"
    # every .so / .dylib under python/ -> a framework (tools/ios_frameworks.py)
    n=$(python3 tools/ios_frameworks.py "$APP" "$BUNDLE_ID" "$IOS_MIN" ${SKELETON:+--skeleton})
    echo "python: $n extension frameworks, $(du -sh "$APP/python" | cut -f1) stdlib + packages, $(du -sh "$APP/rando-gen" | cut -f1) generator"
fi

if [ -n "$SKELETON" ]; then
    # Sideloading tools (Impactor, SideStore, AltStore) take the entitlements to ask Apple for from the executable's
    # own code signature: an ad-hoc signature carrying ours (they re-sign over it). The frameworks stay unsigned
    # (their signature has to be the tool's anyway).
    codesign --force --sign - --entitlements runtime/platform/ios.entitlements --timestamp=none "$APP/lasrecomp"
    codesign -d --entitlements :- "$APP/lasrecomp" 2>/dev/null | grep -q increased-memory-limit || { echo "entitlements not in the signature"; exit 1; }
    echo "$APP (skeleton): $(du -sh "$APP" | cut -f1), $(ls "$APP/Frameworks" | wc -l | tr -d ' ') frameworks, ad-hoc signed with the entitlements"
    exit 0
fi

# ---- signing: the profile (its entitlements must allow ours), then codesign with the team's identity
if [ -n "${PROFILE:-}" ]; then
    cp "$PROFILE" "$APP/embedded.mobileprovision"
    TEAM_ID=${TEAM_ID:-$(security cms -D -i "$PROFILE" | plutil -extract TeamIdentifier.0 raw -o - -)}
fi
if [ -n "${TEAM_ID:-}" ]; then
    if [ -z "${SIGN:-}" ] && [ -n "${PROFILE:-}" ]; then
        # the first certificate of the profile that is also an identity in the keychain (by its CN)
        n=$(security cms -D -i "$PROFILE" | plutil -extract DeveloperCertificates raw -o - -)
        for i in $(seq 0 $((n - 1))); do
            cn=$(security cms -D -i "$PROFILE" | plutil -extract "DeveloperCertificates.$i" raw -o - - | \
                 base64 -d | openssl x509 -inform DER -noout -subject 2>/dev/null | sed 's/.*CN=\([^,]*\).*/\1/')
            [ -n "$cn" ] || continue
            if security find-identity -v -p codesigning | grep -F "\"$cn\"" >/dev/null; then SIGN=$cn; break; fi
        done
    fi
    SIGN=${SIGN:-$(security find-identity -v -p codesigning | grep "Apple Development" | head -1 | sed 's/.*"\(.*\)".*/\1/')}
    [ -n "$SIGN" ] || { echo "no 'Apple Development' identity (SIGN=...)"; exit 1; }
    # the entitlements with the app id filled in (the profile's application-identifier must match)
    ENT=build/ios/entitlements.plist
    python3 - runtime/platform/ios.entitlements "$ENT" "$TEAM_ID" "$BUNDLE_ID" <<'EOF'
import plistlib, sys
with open(sys.argv[1], 'rb') as f: e = plistlib.load(f)
e['application-identifier'] = f'{sys.argv[3]}.{sys.argv[4]}'
e['com.apple.developer.team-identifier'] = sys.argv[3]
e['get-task-allow'] = True
with open(sys.argv[2], 'wb') as f: plistlib.dump(e, f)
EOF
    for f in "$APP"/Frameworks/*.framework; do  # (inside out: every framework, then the app)
        codesign --force --sign "$SIGN" --timestamp=none "$f"
    done
    codesign --force --sign "$SIGN" --entitlements "$ENT" --timestamp=none "$APP"
    echo "signed as $SIGN:"
    codesign -d --entitlements :- "$APP" 2>/dev/null | grep -E "kernel\.|application-identifier" | sed 's/^/  /'
    codesign --verify --deep --strict "$APP" && echo "  verified"
else
    echo "note: unsigned (TEAM_ID / PROFILE not set): sign before installing"
fi

mkdir -p build/dist && rm -rf build/ios/Payload && mkdir build/ios/Payload
cp -R "$APP" build/ios/Payload/
IPA="build/dist/LAS Recomp-$VERSION-ios.ipa"
rm -f "$IPA"
(cd build/ios && zip -qr "../../$IPA" Payload)
rm -rf build/ios/Payload
echo "$IPA ($(du -h "$IPA" | cut -f1))"

if [ -n "${DEVICE:-}" ]; then
    xcrun devicectl device install app --device "$DEVICE" "$APP"
    xcrun devicectl device process launch --device "$DEVICE" --terminate-existing "$BUNDLE_ID"
    echo "log: Documents/lasrecomp.log (Files app, or: xcrun devicectl device copy from --device $DEVICE" \
         "--domain-type appDataContainer --domain-identifier $BUNDLE_ID --source Documents/lasrecomp.log --destination .)"
fi
