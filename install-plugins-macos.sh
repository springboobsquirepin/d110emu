#!/bin/sh
# Installs D110Emu's audio plugins (VST3 and Audio Unit) on a Mac from this folder's build, where DAWs look for them,
# with the ROMs where they find them. ./install-plugins-macos.sh --help lists the options.
set -eu

usage() {
    cat <<'EOF'
Usage: ./install-plugins-macos.sh [options]

Copies the plugins built in this folder into your plugin folders, replacing earlier
copies: the VST3 plugin (bin/macosx-Release/VST3/D110Emu.vst3) into
~/Library/Audio/Plug-Ins/VST3 and the Audio Unit (bin/macosx-Release/AU/D110Emu.component)
into ~/Library/Audio/Plug-Ins/Components, then has macOS register the Audio Unit again.
When the ROM folder ~/Library/Application Support/D110Emu/roms, which D110Emu.app uses
too, has no files yet, the files in this folder's roms folder are copied there.
Quit the DAWs that have a plugin loaded first, and rescan the plugins afterwards.

Options:
  --vst3         Only the VST3 plugin
  --au           Only the Audio Unit
  --build        Build them first: premake5 gmake, then make (it needs premake5 and
                 Xcode's command line tools; see BUILDING.md)
  --debug        The Debug build (bin/macosx-Debug) instead of the Release one
  --system       For every user of the Mac: the plugins into /Library/Audio/Plug-Ins,
                 the ROMs into /Library/Audio/Plug-Ins/roms (sudo asks for your password)
  --roms DIR     Copy the files in DIR into the ROM folder (files already there stay)
  --no-roms      Leave the ROMs alone
  --roms-inside  Also put the ROMs inside the installed plugins and sign them again, for
                 hosts that cannot read your folders (GarageBand, if D110Emu finds no ROMs)
  --validate     Run Apple's Audio Unit validation (auval) on the installed Audio Unit
  --uninstall    Remove the installed plugins; the ROMs and settings stay
  -h, --help     This text
EOF
}

die() {
    echo "$*" >&2
    exit 1
}

# The options come first, so that --roms takes a folder relative to where the script was started.
config=Release
make_config=release
vst3=0
au=0
build=0
system=0
uninstall=0
roms_from=
no_roms=0
roms_inside=0
validate=0
while [ $# -gt 0 ]; do
    case $1 in
        --vst3) vst3=1 ;;
        --au) au=1 ;;
        --build) build=1 ;;
        --debug) config=Debug; make_config=debug ;;
        --system) system=1 ;;
        --roms)
            [ $# -ge 2 ] || die "--roms needs a folder."
            [ -d "$2" ] || die "No folder $2"
            roms_from=$(cd "$2" && pwd)
            shift ;;
        --no-roms) no_roms=1 ;;
        --roms-inside) roms_inside=1 ;;
        --validate) validate=1 ;;
        --uninstall) uninstall=1 ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; die "Unknown option: $1" ;;
    esac
    shift
done
[ -z "$roms_from" ] || [ "$no_roms" -eq 0 ] || die "--roms and --no-roms do not go together."
[ "$roms_inside" -eq 0 ] || [ "$no_roms" -eq 0 ] || die "--roms-inside and --no-roms do not go together."
# Neither --vst3 nor --au: both, as far as they are built.
chosen=1
if [ "$vst3" -eq 0 ] && [ "$au" -eq 0 ]; then
    vst3=1
    au=1
    chosen=0
fi

[ "$(uname -s)" = Darwin ] || die "This script is for macOS; on Linux, use install-plugins-linux.sh."
cd "$(dirname "$0")"  # The d110emu folder, where the build is

if [ "$system" -eq 1 ]; then
    plugins=/Library/Audio/Plug-Ins
    rom_dir=$plugins/roms  # Above both bundles, where the plugins look for every user
else
    plugins=$HOME/Library/Audio/Plug-Ins
    rom_dir="$HOME/Library/Application Support/D110Emu/roms"
fi
vst3_built=bin/macosx-$config/VST3/D110Emu.vst3
au_built=bin/macosx-$config/AU/D110Emu.component
vst3_installed=$plugins/VST3/D110Emu.vst3
au_installed=$plugins/Components/D110Emu.component

# The folders for every user belong to root: what writes there goes through sudo.
if [ "$system" -eq 1 ] && [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
    die "Installing for every user needs root: run this as root, or install sudo."
fi
as_installer() {
    if [ "$system" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
        sudo "$@"
    else
        "$@"
    fi
}

# macOS keeps a list of Audio Units: its registrar starts again when needed and finds a new or changed one at once.
register_audio_units() {
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
}

if [ "$uninstall" -eq 1 ]; then
    for installed in "$vst3_installed" "$au_installed"; do
        case $installed in
            *.vst3) [ "$vst3" -eq 1 ] || continue ;;
            *) [ "$au" -eq 1 ] || continue ;;
        esac
        if [ -e "$installed" ]; then
            as_installer rm -rf "$installed"
            echo "Removed $installed"
        else
            echo "Nothing to remove: $installed is not there."
        fi
    done
    if [ "$au" -eq 1 ]; then register_audio_units; fi
    echo "The ROMs ($rom_dir) and the settings stay."
    exit 0
fi

if [ "$build" -eq 1 ]; then
    # premake5 makes the makefiles (BUILDING.md), again each time, as the sources may have changed.
    if command -v premake5 >/dev/null 2>&1; then
        echo "Making the makefiles: premake5 gmake"
        premake5 gmake
    elif [ ! -f build/gmake-macosx/Makefile ]; then
        die "premake5 makes the makefiles, and it is not installed: see BUILDING.md."
    fi
    targets=
    if [ "$vst3" -eq 1 ]; then targets="$targets D110EmuVST3"; fi
    if [ "$au" -eq 1 ]; then targets="$targets D110EmuAU"; fi
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    echo "Building: make -C build/gmake-macosx config=$make_config$targets -j$jobs"
    # shellcheck disable=SC2086  # The targets are separate words
    make -C build/gmake-macosx config="$make_config" $targets -j"$jobs"
fi

# What is built: a format asked for by name must be; otherwise one that is not is left out.
build_hint() {
    echo "  premake5 gmake"
    echo "  make -C build/gmake-macosx config=$make_config $1 -j8"
}
if [ "$vst3" -eq 1 ] && [ ! -f "$vst3_built/Contents/MacOS/D110Emu" ]; then
    [ "$chosen" -eq 0 ] || die "The VST3 plugin is not built yet ($vst3_built). Build it as BUILDING.md says:
$(build_hint D110EmuVST3)
or run this script with --build."
    vst3=0
    echo "Not built, left out: the VST3 plugin ($vst3_built)"
fi
if [ "$au" -eq 1 ] && [ ! -f "$au_built/Contents/MacOS/D110Emu" ]; then
    [ "$chosen" -eq 0 ] || die "The Audio Unit is not built yet ($au_built). Build it as BUILDING.md says:
$(build_hint D110EmuAU)
or run this script with --build."
    au=0
    echo "Not built, left out: the Audio Unit ($au_built)"
fi
[ "$vst3" -eq 1 ] || [ "$au" -eq 1 ] || die "Neither plugin is built yet. Build them as BUILDING.md says:
$(build_hint "D110EmuVST3 D110EmuAU")
or run this script with --build."

# Copies a built bundle over an installed one. ditto keeps its signature; a copy that came from another Mac (a
# download) carries macOS's download mark, which keeps hosts from loading a plugin not signed by a developer ID.
install_bundle() {
    as_installer mkdir -p "${2%/*}"
    as_installer rm -rf "$2"  # An earlier copy goes whole, so no old file stays behind
    as_installer ditto "$1" "$2"
    as_installer xattr -dr com.apple.quarantine "$2" >/dev/null 2>&1 || true
    echo "Installed $2"
}

if [ "$vst3" -eq 1 ]; then install_bundle "$vst3_built" "$vst3_installed"; fi
if [ "$au" -eq 1 ]; then install_bundle "$au_built" "$au_installed"; fi

# Whether a folder holds at least one file.
has_files() {
    for entry in "$1"/*; do
        if [ -f "$entry" ]; then return 0; fi
    done
    return 1
}

# Copies the files of a folder into another, keeping any file of the same name already there.
copy_files() {
    as_installer mkdir -p "$2"
    copied=0
    kept=0
    for file in "$1"/*; do
        if [ ! -f "$file" ]; then continue; fi
        if [ -e "$2/${file##*/}" ]; then
            kept=$((kept + 1))
            continue
        fi
        as_installer cp "$file" "$2/"
        copied=$((copied + 1))
    done
    echo "ROMs: $copied file(s) copied from $1 into $2 ($kept already there)"
}

if [ "$no_roms" -eq 0 ]; then
    if [ -n "$roms_from" ]; then
        copy_files "$roms_from" "$rom_dir"
    elif has_files "$rom_dir"; then
        echo "ROMs: the plugins find those in $rom_dir"
    elif has_files roms; then
        copy_files "$(pwd)/roms" "$rom_dir"
    else
        echo "ROMs: none found. Put the D-110's ROM images (see the README's ROMs section) into"
        echo "  $rom_dir"
        echo "or choose their folder in a plugin's File > Configuration."
    fi
    if [ "$roms_inside" -eq 1 ]; then
        # Inside a bundle the ROMs change what its signature covers: it is signed again (ad hoc, as the build signs it).
        has_files "$rom_dir" || die "No ROMs to put inside the plugins: $rom_dir has none."
        for installed in "$vst3_installed" "$au_installed"; do
            case $installed in
                *.vst3) [ "$vst3" -eq 1 ] || continue ;;
                *) [ "$au" -eq 1 ] || continue ;;
            esac
            copy_files "$rom_dir" "$installed/Contents/Resources/roms"
            as_installer codesign --force --sign - "$installed"
        done
    fi
fi

for installed in "$vst3_installed" "$au_installed"; do
    case $installed in
        *.vst3) [ "$vst3" -eq 1 ] || continue ;;
        *) [ "$au" -eq 1 ] || continue ;;
    esac
    codesign --verify "$installed" >/dev/null 2>&1 || echo "Warning: the signature of $installed does not verify; hosts may refuse it."
done

if [ "$au" -eq 0 ] && [ "$validate" -eq 1 ]; then
    echo "No Audio Unit installed: nothing to validate."
fi
if [ "$au" -eq 1 ]; then
    register_audio_units
    if [ "$validate" -eq 1 ]; then
        log=${TMPDIR:-/tmp}/d110emu-auval.txt
        echo "Validating the Audio Unit (auval -v aumu D110 LA32)..."
        sleep 1  # The registrar starts again
        if auval -v aumu D110 LA32 >"$log" 2>&1 && grep -q "AU VALIDATION SUCCEEDED" "$log"; then
            echo "Audio Unit validation: passed (the whole output is in $log)"
        else
            echo "Audio Unit validation: FAILED; see $log" >&2
            exit 1
        fi
    fi
fi

echo "Done: rescan the plugins in your DAW (one that had a plugin loaded needs a restart)."
if [ "$au" -eq 1 ]; then
    echo "Logic Pro and GarageBand check a new Audio Unit when they start; GarageBand asks before it loads D110Emu:"
    echo "let it lower its security settings."
fi
