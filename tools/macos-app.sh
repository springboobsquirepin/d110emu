#!/bin/sh
# The macOS bundles' build steps (D110Emu.app, D110Emu.vst3, D110Emu.component), which the makefiles in
# build/gmake-macosx run (see premake5.lua):
#
#   macos-app.sh sdl3 DIR
#       Puts SDL 3's official macOS framework in DIR/SDL3.framework (arm64 for macOS 11 and later, x86_64 for 10.13),
#       from the release image SDL publishes, downloaded once and checked against its SHA-256. The apps are compiled and
#       linked against it; when they build at once, one of them fetches it and the others wait.
#   macos-app.sh bundle APP PLIST FRAMEWORK [ICON]
#       Makes APP (D110Emu.app, its program just linked) whole: its Info.plist, SDL3.framework in Contents/Frameworks
#       (so that it runs on Macs that have no SDL) and the icon (an .icns) in Contents/Resources; then signs it all ad
#       hoc, and registers it with LaunchServices again, so that Finder and the Dock show what it is now.
#   macos-app.sh plugin BUNDLE PLIST
#       Makes BUNDLE (D110Emu.vst3, its program just linked) whole: its Info.plist and PkgInfo; then signs it ad hoc.

set -e

SDL_VERSION=3.4.16
SDL_SHA256=675660a9e457239af615f9e41f788612168d1639b9d2eda2957e8dace26687fd
SDL_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.dmg

sha256() {
    if command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d ' ' -f 1
    else
        openssl dgst -sha256 -r "$1" | cut -d ' ' -f 1
    fi
}

case "$1" in
sdl3)
    dir=$2
    framework=$dir/SDL3.framework
    ready() {
        [ -f "$framework/Versions/A/SDL3" ] && [ "$(cat "$dir/version" 2> /dev/null)" = "$SDL_VERSION" ]
    }
    if ready; then
        exit 0
    fi
    mkdir -p "$dir"
    # The apps (D110Emu, MT32Translator, ToneEditor) build side by side with make -j: one of them puts SDL there, the
    # others wait for it (mkdir makes the lock, as it fails when the folder is there). A lock left by a build that was
    # stopped is taken over after five minutes.
    lock=$dir/lock
    waited=0
    until mkdir "$lock" 2> /dev/null; do
        if [ "$waited" -ge 300 ]; then
            echo "note: taking over $lock, left by an earlier build" >&2
            rmdir "$lock" 2> /dev/null || true
            waited=0
            continue
        fi
        sleep 1
        waited=$((waited + 1))
    done
    trap 'rmdir "$lock" 2> /dev/null' EXIT INT TERM
    if ready; then
        exit 0
    fi
    rm -f "$dir/version"  # Written when the framework is whole again
    image=$dir/SDL3-$SDL_VERSION.dmg
    if [ ! -f "$image" ] || [ "$(sha256 "$image")" != "$SDL_SHA256" ]; then
        echo "Downloading SDL $SDL_VERSION for the window (once): $SDL_URL"
        rm -f "$image"
        curl -fL --retry 3 -o "$image.part" "$SDL_URL"
        mv "$image.part" "$image"
        sum=$(sha256 "$image")
        if [ "$sum" != "$SDL_SHA256" ]; then
            echo "error: the download is not SDL $SDL_VERSION's release image (its SHA-256 is $sum)" >&2
            rm -f "$image"
            exit 1
        fi
    fi
    # The image holds SDL3.xcframework for all of Apple's systems; the Mac's framework is copied out of it as it is
    # (ditto keeps its links and its signature).
    mountpoint=$(mktemp -d)
    hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mountpoint" "$image" < /dev/null > /dev/null
    rm -rf "$framework"
    status=0
    ditto "$mountpoint/SDL3.xcframework/macos-arm64_x86_64/SDL3.framework" "$framework" || status=$?
    hdiutil detach "$mountpoint" > /dev/null || true
    rmdir "$mountpoint" 2> /dev/null || true
    if [ "$status" -ne 0 ]; then
        exit "$status"
    fi
    echo "$SDL_VERSION" > "$dir/version"
    ;;
bundle)
    app=$2
    plist=$3
    framework=$4
    cp -f "$plist" "$app/Contents/Info.plist"
    rm -rf "$app/Contents/Frameworks/SDL3.framework"
    mkdir -p "$app/Contents/Frameworks"
    ditto "$framework" "$app/Contents/Frameworks/SDL3.framework"
    if [ -n "$5" ]; then
        mkdir -p "$app/Contents/Resources"
        cp -f "$5" "$app/Contents/Resources/"
    fi
    # macOS checks an app as a whole: its program, its Info.plist and SDL3.framework (which keeps SDL's own signature).
    # Unsigned, or changed after it was signed, a downloaded app is refused as damaged. codesign refuses files that carry
    # Finder information, which files copied from another computer may have.
    xattr -cr "$app" 2> /dev/null || true
    codesign --force --sign - "$app"
    # macOS keeps what it knows of an app, its icon among it, by the app's path until the app looks changed, and an app
    # rebuilt in place does not: the Dock then goes on showing the old icon. Registering it again, as Xcode does after
    # every build, makes it read the app anew (lsregister comes with macOS, not with Xcode).
    touch "$app"
    lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
    if [ -x "$lsregister" ]; then
        "$lsregister" -f "$app" || true
    fi
    ;;
plugin)
    bundle=$2
    plist=$3
    cp -f "$plist" "$bundle/Contents/Info.plist"
    printf 'BNDL????' > "$bundle/Contents/PkgInfo"
    # Signed as a whole, as the app is: Apple silicon runs no unsigned code, and a bundle's signature covers its
    # Info.plist.
    xattr -cr "$bundle" 2> /dev/null || true
    codesign --force --sign - "$bundle"
    ;;
*)
    echo "usage: $0 sdl3 DIR | bundle APP PLIST FRAMEWORK [ICON] | plugin BUNDLE PLIST" >&2
    exit 2
    ;;
esac
