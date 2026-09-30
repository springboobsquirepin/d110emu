#!/bin/sh
# Installs D110Emu's VST3 plugin on Linux from this folder's build, where DAWs look for it, with the ROMs where it finds
# them. ./install-plugins-linux.sh --help lists the options.
set -eu

usage() {
    cat <<'EOF'
Usage: ./install-plugins-linux.sh [options]

Copies the VST3 plugin built in this folder (bin/linux-Release/VST3/D110Emu.vst3) into
~/.vst3, where DAWs look for VST3 plugins, replacing an earlier copy. When the ROM
folder ~/.local/share/D110Emu/roms ($XDG_DATA_HOME/D110Emu/roms when that is set),
which the D110Emu programs use too, has no files yet, the files in this folder's roms
folder are copied there. Rescan the plugins in the DAW afterwards.

Options:
  --build       Build the plugin first: premake5 gmake, then make ... D110EmuVST3 (it
                needs premake5, build-essential, libx11-dev and libgl-dev; see
                BUILDING.md)
  --debug       The Debug build (bin/linux-Debug) instead of the Release one
  --system      For every user of the computer: the plugin into /usr/local/lib/vst3,
                the ROMs into /usr/local/lib/vst3/roms (sudo asks for your password)
  --roms DIR    Copy the files in DIR into the ROM folder (files already there stay)
  --no-roms     Leave the ROMs alone
  --uninstall   Remove the installed plugin (with --system: the one for every user);
                the ROMs and settings stay
  -h, --help    This text
EOF
}

die() {
    echo "$*" >&2
    exit 1
}

# The options come first, so that --roms takes a folder relative to where the script was started.
config=Release
make_config=release
build=0
system=0
uninstall=0
roms_from=
no_roms=0
while [ $# -gt 0 ]; do
    case $1 in
        --build) build=1 ;;
        --debug) config=Debug; make_config=debug ;;
        --system) system=1 ;;
        --roms)
            [ $# -ge 2 ] || die "--roms needs a folder."
            [ -d "$2" ] || die "No folder $2"
            roms_from=$(cd "$2" && pwd)
            shift ;;
        --no-roms) no_roms=1 ;;
        --uninstall) uninstall=1 ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; die "Unknown option: $1" ;;
    esac
    shift
done
[ -z "$roms_from" ] || [ "$no_roms" -eq 0 ] || die "--roms and --no-roms do not go together."

[ "$(uname -s)" = Linux ] || die "This script is for Linux; on a Mac, use install-plugins-macos.sh."
cd "$(dirname "$0")"  # The d110emu folder, where the build is

if [ "$system" -eq 1 ]; then
    plugin_dir=/usr/local/lib/vst3
    rom_dir=$plugin_dir/roms  # Above the bundle, where the plugin looks for every user
else
    plugin_dir=$HOME/.vst3
    rom_dir=${XDG_DATA_HOME:-$HOME/.local/share}/D110Emu/roms
fi
installed=$plugin_dir/D110Emu.vst3

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

if [ "$uninstall" -eq 1 ]; then
    if [ -e "$installed" ]; then
        as_installer rm -rf "$installed"
        echo "Removed $installed"
    else
        echo "Nothing to remove: $installed is not there."
    fi
    echo "The ROMs ($rom_dir) and the settings stay."
    exit 0
fi

if [ "$build" -eq 1 ]; then
    # premake5 makes the makefiles (BUILDING.md), again each time, as the sources may have changed.
    if command -v premake5 >/dev/null 2>&1; then
        echo "Making the makefiles: premake5 gmake"
        premake5 gmake
    elif [ ! -f build/gmake-linux/Makefile ]; then
        die "premake5 makes the makefiles, and it is not installed: see BUILDING.md."
    fi
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    echo "Building: make -C build/gmake-linux config=$make_config D110EmuVST3 -j$jobs"
    make -C build/gmake-linux config="$make_config" D110EmuVST3 -j"$jobs"
fi

bundle=bin/linux-$config/VST3/D110Emu.vst3
arch=$(uname -m)
if [ ! -f "$bundle/Contents/$arch-linux/D110Emu.so" ]; then
    [ ! -d "$bundle" ] || die "$bundle holds no plugin for this computer ($arch-linux): build it here, with --build."
    die "The plugin is not built yet ($bundle is not there). Build it as BUILDING.md says:
  premake5 gmake
  make -C build/gmake-linux config=$make_config D110EmuVST3 -j4
or run this script with --build."
fi

as_installer mkdir -p "$plugin_dir"
as_installer rm -rf "$installed"  # An earlier copy goes whole, so no old file stays behind
as_installer cp -R "$bundle" "$installed"
echo "Installed $installed"

# Whether a folder holds at least one file.
has_files() {
    for entry in "$1"/*; do
        if [ -f "$entry" ]; then return 0; fi
    done
    return 1
}

# Copies the files of a folder into the ROM folder, keeping any file of the same name already there.
copy_roms() {
    as_installer mkdir -p "$rom_dir"
    copied=0
    kept=0
    for file in "$1"/*; do
        if [ ! -f "$file" ]; then continue; fi
        if [ -e "$rom_dir/${file##*/}" ]; then
            kept=$((kept + 1))
            continue
        fi
        as_installer cp "$file" "$rom_dir/"
        copied=$((copied + 1))
    done
    echo "ROMs: $copied file(s) copied from $1 into $rom_dir ($kept already there)"
}

if [ "$no_roms" -eq 0 ]; then
    if [ -n "$roms_from" ]; then
        copy_roms "$roms_from"
    elif has_files "$rom_dir"; then
        echo "ROMs: the plugin finds those in $rom_dir"
    elif has_files roms; then
        copy_roms "$(pwd)/roms"
    else
        echo "ROMs: none found. Put the D-110's ROM images (see the README's ROMs section) into"
        echo "  $rom_dir"
        echo "or choose their folder in the plugin's File > Configuration."
    fi
fi

echo "Done: rescan the plugins in your DAW (a DAW that had the plugin loaded needs a restart)."
