# Building D110Emu

This file says how to build D110Emu from its sources on Windows, Linux and macOS: the emulator (its window and its terminal version), its plugins, the programs for real units (MT32Translator, ToneEditor and PatternCapture), and the tools and tests. [README.md](README.md) says how to use them.

The sources come without project files, so a build has the same two parts on every system. First, premake5 turns `premake5.lua` into project files for your system: a Visual Studio solution on Windows, makefiles on Linux and macOS. Then Visual Studio or make builds from those. Run every command here in the `d110emu` folder, the one that holds `premake5.lua`.

The ROMs are not part of the sources. Before the first start, put them where [README.md](README.md#roms) says; a `roms` folder next to `bin` does for everything built here.

## What gets built

| | Windows, in `bin\Release` | Linux, in `bin/linux-Release` | macOS, in `bin/macosx-Release` |
|---|---|---|---|
| D110Emu | `D110Emu.exe` | `d110emu` | `D110Emu.app` |
| D110Emu's terminal version | `D110EmuTUI.exe` | `d110emu-tui` | `d110emu-tui` |
| VST2 plugin | `VST2\D110Emu.dll` | | |
| VST3 plugin | `VST3\D110Emu.vst3` | `VST3/D110Emu.vst3` | `VST3/D110Emu.vst3` |
| Audio Unit | | | `AU/D110Emu.component` |
| MT32Translator | `MT32Translator.exe` | `mt32translator` | `MT32Translator.app` |
| MT32Translator's terminal version | `MT32TranslatorTUI.exe` | `mt32translator-tui` | `mt32translator-tui` |
| ToneEditor | `ToneEditor.exe` | `toneeditor` | `ToneEditor.app` |
| PatternCapture | `PatternCapture.exe` | | |

The tools and tests go in the same folder (see [Tests and tools](#tests-and-tools)). Debug builds go in `bin\Debug`, `bin/linux-Debug` and `bin/macosx-Debug`.

## Windows

### Prerequisites

- Windows 10 or 11, 64-bit.
- Visual Studio 2022 with the **Desktop development with C++** workload. The free Community edition does.
- premake5 comes with the sources, as `tools\premake5.exe` (version 5.0.0-beta8), so there is nothing to install for it.

The sources carry every library they use (Dear ImGui, miniaudio, the VST3 interfaces), so nothing else is needed.

### 1. Generate the project files

Run `generate_vs2022.bat`: double-click it in Explorer, or type it in a Command Prompt in the `d110emu` folder. It runs `tools\premake5.exe vs2022`, which writes `D110Emu.sln` and the projects in `build\vs2022`.

### 2. Build

1. Open `D110Emu.sln` in Visual Studio 2022.
2. Choose **Release** and **x64** in the toolbar. Debug builds play in real time too, as the emulation core is compiled with optimisations in both.
3. **Build > Build Solution** (Ctrl+Shift+B) builds everything. To build one program or plugin, right-click its project in the Solution Explorer and choose **Build**.

**Debug > Start Without Debugging** (Ctrl+F5) starts D110Emu. To start another program from Visual Studio, right-click its project and choose **Set as Startup Project**, or run it from `bin\Release`. The programs link the C runtime statically, so an .exe can be copied to another PC with Windows 10 or 11, along with a `roms` folder.

### 3. Install the plugins

- **VST2**: point the DAW's VST2 folder at `bin\Release\VST2`, or copy `D110Emu.dll` into a folder the DAW scans. Then rescan in the DAW.
- **VST3**: copy the whole `bin\Release\VST3\D110Emu.vst3` folder into `C:\Program Files\Common Files\VST3`, or another folder your DAW scans for VST3 plugins. Then rescan in the DAW.

A plugin under Program Files finds its ROMs in `%APPDATA%\D110Emu\roms` or inside its bundle ([README.md](README.md#vst3-plugin) has the places).

## Linux

### Prerequisites

Any distribution with GCC and, for the windows, SDL 3.2 or later, on a 64-bit PC or an ARM computer (a Raspberry Pi 4 or 5 included). A build needs these packages:

| What for | Debian, Ubuntu, Raspberry Pi OS | Fedora | RHEL, AlmaLinux, Rocky Linux | Arch Linux, Manjaro |
|---|---|---|---|---|
| Everything: a C++ compiler and make | `build-essential` | `gcc-c++ make` | `gcc-c++ make` | `base-devel` |
| Everything: premake5 | none, see [below](#premake5-where-the-distribution-has-none) | `premake` | none, see [below](#premake5-where-the-distribution-has-none) | `premake` |
| The windows: SDL 3 | `libsdl3-dev` | `SDL3-devel` | `SDL3-devel` (CRB) | `sdl3` |
| The VST3 plugin: X11, OpenGL and the static C++ runtime | `libx11-dev libgl-dev` | `libX11-devel libglvnd-devel libstdc++-static` | `libX11-devel libglvnd-devel libstdc++-static` (CRB) | `libx11 libglvnd` |

The terminal versions and the tools need only the first two rows (but vst3host, the test host for the plugin, needs X11 too). Sound (PipeWire, PulseAudio or ALSA) and MIDI (the ALSA sequencer) need no development files, as the programs load them when they run. The plugin carries its own copy of the C++ runtime, so that it never depends on the one a DAW brings. It takes that copy from the static C++ runtime, which Fedora and RHEL keep in a package of its own, `libstdc++-static`: without it, the plugin's link stops with "cannot find -lstdc++".

#### Debian, Ubuntu and Raspberry Pi OS

Debian 13 or later, Raspberry Pi OS based on it, or Ubuntu 25.04 or later (earlier releases have no SDL 3).

```sh
sudo apt install build-essential           # everything
sudo apt install libsdl3-dev               # the windows
sudo apt install libx11-dev libgl-dev      # the VST3 plugin (libsdl3-dev brings these too)
```

These distributions have no premake5 (Debian's `premake4` cannot make these project files): see [premake5 where the distribution has none](#premake5-where-the-distribution-has-none).

#### Fedora

Fedora 43 or later:

```sh
sudo dnf install gcc-c++ make premake                            # everything (premake gives premake5)
sudo dnf install SDL3-devel                                      # the windows
sudo dnf install libX11-devel libglvnd-devel libstdc++-static    # the VST3 plugin
```

`SDL3-devel` brings `libX11-devel` and `libglvnd-devel` too, but not `libstdc++-static`.

#### RHEL, AlmaLinux, Rocky Linux and CentOS Stream

Version 10 (version 9 has no SDL 3). `SDL3-devel` and `libstdc++-static` are in the CRB repository (CodeReady Linux Builder on RHEL), which is off until you turn it on:

```sh
sudo dnf config-manager --set-enabled crb                                             # AlmaLinux, Rocky Linux, CentOS Stream
sudo subscription-manager repos --enable codeready-builder-for-rhel-10-$(arch)-rpms   # RHEL itself
```

Then:

```sh
sudo dnf install gcc-c++ make                                    # everything
sudo dnf install SDL3-devel                                      # the windows
sudo dnf install libX11-devel libglvnd-devel libstdc++-static    # the VST3 plugin
```

These distributions have no premake5, EPEL included: see [premake5 where the distribution has none](#premake5-where-the-distribution-has-none).

#### Arch Linux, Manjaro and EndeavourOS

```sh
sudo pacman -S --needed base-devel premake    # everything
sudo pacman -S --needed sdl3                  # the windows
sudo pacman -S --needed libx11 libglvnd       # the VST3 plugin
```

Arch's packages include their development files, and its `gcc` has the static C++ runtime.

#### premake5 where the distribution has none

The commands below install premake5 5.0.0-beta8 (`premake5 --version` shows the version) into `~/.local/bin`, which most distributions add to the PATH once the folder exists: log in again, or type `~/.local/bin/premake5` until then.

- On a 64-bit Intel or AMD PC, take premake's ready-made one:

  ```sh
  curl -LO https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz
  tar -xzf premake-5.0.0-beta8-linux.tar.gz
  chmod +x premake5 && mkdir -p ~/.local/bin && mv premake5 ~/.local/bin/
  ```

- On a Raspberry Pi or another ARM computer, premake has no ready-made one, so build it from its sources (a minute on a Raspberry Pi 5). It needs the UUID library's development files:

  ```sh
  sudo apt install uuid-dev          # Debian, Ubuntu, Raspberry Pi OS
  sudo dnf install libuuid-devel     # RHEL, AlmaLinux, Rocky Linux
  curl -L https://github.com/premake/premake-core/archive/refs/tags/v5.0.0-beta8.tar.gz | tar -xz
  make -C premake-core-5.0.0-beta8 -f Bootstrap.mak linux
  mkdir -p ~/.local/bin && cp premake-core-5.0.0-beta8/bin/release/premake5 ~/.local/bin/
  ```

- With Homebrew on Linux, `brew install premake` gives it on both.

### 1. Generate the makefiles

```sh
premake5 gmake
```

It writes the makefiles in `build/gmake-linux`.

### 2. Build

```sh
make -C build/gmake-linux config=release D110Emu D110EmuTUI -j4                     # the window and the terminal version
make -C build/gmake-linux config=release MT32Translator MT32TranslatorTUI ToneEditor -j4
make -C build/gmake-linux config=release D110EmuVST3 -j4                           # the VST3 plugin
make -C build/gmake-linux config=release -j4                                       # everything, the tools and tests too
```

Each line builds what it names and what that needs. `-j4` compiles four files at once (`-j$(nproc)` uses every core). On a Raspberry Pi 5 the window and the terminal version take about two minutes, everything about nine. The terminal versions build without SDL 3 (`make -C build/gmake-linux config=release D110EmuTUI MT32TranslatorTUI -j4`). `config=debug` makes a Debug build, in `bin/linux-Debug`.

The programs are then in `bin/linux-Release`:

```sh
./bin/linux-Release/d110emu        # the window
./bin/linux-Release/d110emu-tui    # the terminal version
```

### 3. Install the VST3 plugin

```sh
./install-plugins-linux.sh
```

The script copies `bin/linux-Release/VST3/D110Emu.vst3` into `~/.vst3`, where DAWs look for VST3 plugins, replacing an earlier copy whole. When `~/.local/share/D110Emu/roms` (the ROM folder D110Emu's own programs use too) has no files yet, it copies the files of the `roms` folder there. Then rescan in the DAW. Its options:

- `--build` builds the plugin first (`premake5 gmake`, then make);
- `--system` installs it for every user of the computer, into `/usr/local/lib/vst3` with its ROMs beside it (through sudo);
- `--roms DIR` takes the ROMs from another folder, and `--no-roms` leaves them alone;
- `--uninstall` removes the plugin again;
- `--help` lists everything.

Run it as `sh install-plugins-linux.sh` if the folder's copy lost its permission to run. Without the script, copy the whole `D110Emu.vst3` folder into `~/.vst3` yourself (or `/usr/local/lib/vst3` for every user).

## macOS

### Prerequisites

macOS 11 or later, on Apple silicon or an Intel Mac. The build makes programs for the kind of Mac it runs on.

1. Apple's command line tools: the compiler and make. Xcode brings them; without Xcode:

   ```sh
   xcode-select --install
   ```

2. premake5, version 5.0.0-beta8 or later (`premake5 --version` shows the version):
   - with [Homebrew](https://brew.sh):

     ```sh
     brew install premake
     ```

   - or premake's ready-made one, `premake-5.0.0-beta8-macosx.tar.gz` for Apple silicon (`premake-5.0.0-beta8-macosx-x64.tar.gz` for an Intel Mac):

     ```sh
     curl -LO https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-macosx.tar.gz
     tar -xzf premake-5.0.0-beta8-macosx.tar.gz
     chmod +x premake5 && sudo mkdir -p /usr/local/bin && sudo mv premake5 /usr/local/bin/
     ```

     One downloaded with a web browser instead carries macOS's download mark, and macOS won't run it until `xattr -d com.apple.quarantine premake5` removes the mark.

3. An internet connection for the first build of D110Emu, MT32Translator or ToneEditor. It downloads SDL 3's official framework for the Mac once (27 MB, from SDL's releases on GitHub, checked against its SHA-256) into `build/sdl3-macos`, and every build puts it inside the apps.

Nothing else is needed. The apps and plugins carry what they use, so they run on Macs without Homebrew or SDL.

### 1. Generate the makefiles

```sh
premake5 gmake
```

It writes the makefiles in `build/gmake-macosx`.

### 2. Build

```sh
make -C build/gmake-macosx config=release D110Emu D110EmuTUI -j8                   # the app and the terminal version
make -C build/gmake-macosx config=release MT32Translator MT32TranslatorTUI ToneEditor -j8
make -C build/gmake-macosx config=release D110EmuVST3 D110EmuAU -j8               # the VST3 plugin and the Audio Unit
make -C build/gmake-macosx config=release -j8                                     # everything, the tools and tests too
```

`-j8` compiles eight files at once (`-j$(sysctl -n hw.ncpu)` uses every core). `config=debug` makes a Debug build, in `bin/macosx-Debug`. The apps and plugins are signed ad hoc as they are built, as macOS requires.

The programs are then in `bin/macosx-Release`:

```sh
open bin/macosx-Release/D110Emu.app     # the window (or double-click it in Finder)
./bin/macosx-Release/d110emu-tui        # the terminal version
```

### 3. Install the VST3 plugin and the Audio Unit

```sh
./install-plugins-macos.sh --validate
```

The script copies the VST3 plugin into `~/Library/Audio/Plug-Ins/VST3` and the Audio Unit into `~/Library/Audio/Plug-Ins/Components` (whichever of them is built), replacing earlier copies whole, and removes macOS's download mark from them. It has macOS register the Audio Unit again at once (otherwise that may take a restart). When `~/Library/Application Support/D110Emu/roms` (the ROM folder D110Emu.app uses too) has no files yet, it copies the files of the `roms` folder there. `--validate` then runs Apple's validation on the Audio Unit (`auval`), the one Logic Pro and GarageBand run before they use a new plugin; it ends with **AU VALIDATION SUCCEEDED**. Quit the DAWs that have one of the plugins loaded first, and rescan afterwards. Its other options:

- `--vst3` or `--au` installs only that one;
- `--build` builds them first (`premake5 gmake`, then make);
- `--roms-inside` also puts the ROMs inside the installed plugins and signs them again, for hosts that cannot read your folders (GarageBand, if D110Emu finds no ROMs there);
- `--system` installs them for every user of the Mac, into `/Library/Audio/Plug-Ins` with the ROMs in its `roms` folder (through sudo);
- `--roms DIR`, `--no-roms` and `--uninstall` work as on Linux, and `--help` lists everything.

Run it as `sh install-plugins-macos.sh` if the folder's copy lost its permission to run. Without the script, the same by hand, after removing an earlier copy (`ditto` adds to a folder that is there already):

```sh
ditto bin/macosx-Release/VST3/D110Emu.vst3 ~/Library/Audio/Plug-Ins/VST3/D110Emu.vst3
ditto bin/macosx-Release/AU/D110Emu.component ~/Library/Audio/Plug-Ins/Components/D110Emu.component
killall -9 AudioComponentRegistrar
auval -v aumu D110 LA32
```

## After updating the sources

- Run premake5 again (step 1 for your system), as a newer version may add or remove source files.
- make rebuilds only what changed in the sources, not what changed in how they are compiled. If a build fails after an update in a way the changes don't explain ("recompile with -fPIC", say), clean first with `make -C build/gmake-linux config=release clean` (on a Mac, `build/gmake-macosx`). A plain `make clean` cleans the Debug build. In Visual Studio, **Build > Rebuild Solution** does the same.

## Tests and tools

Building everything (the whole solution, or make without a target) also builds these, next to the programs:

| Tool | What it does |
|---|---|
| `d110tests` | Checks the emulation against the ROMs (and the terminal version's keys, screen and MIDI input), printing PASS or FAIL for each test. Its exit code is 0 when all pass. Run it in the `d110emu` folder, with the ROMs in `roms`. |
| `vsttest` (Windows and Linux) | Drives the VST2 plugin as a host does: its timing, its outputs, its state in a project, the host's sample rate. |
| `vst3test` | Drives the VST3 plugin the same way: its outputs, its MIDI mapping, its state. With `--module`, it loads a built plugin as a DAW does: `--module bin/linux-Release/VST3/D110Emu.vst3/Contents/aarch64-linux/D110Emu.so` on Linux (`x86_64-linux` on a PC), `--module bin/macosx-Release/VST3/D110Emu.vst3` on a Mac. |
| `d110render` | Renders a MIDI file through the emulator into a WAV file, without audio hardware (below). |
| `mt32translate` | MT32Translator's file translation, on the command line ([README.md](README.md#files) has its options). |
| `presetpitch` | Renders the MT-32's presets against their D-110 stand-ins (it needs the MT-32 and the D-110 ROMs). |
| `uisnap`, `tuisnap` (Linux and macOS) | Screenshots of the windows, and the terminal versions' screens as text, without a display. |
| `vst3host` (Linux) | A small VST3 host that opens the built plugin's window. |

`d110render song.mid out.wav` renders a MIDI file. Other options:
- `--test` plays a chord on every part, then a drum pattern.
- `--syx dump.syx` loads a SysEx file first.
- `--mt32` turns on MT-32 translation, for an MT-32 song and its SysEx files.
- `--syx d20.syx --d20-track` (or `--d20-pattern P-51`) renders a D-20 dump's rhythm track or a pattern; `--tempo` sets the tempo.
- `--rom-song 3` renders a ROM Play song.
- `--export-rom-songs DIR` writes all ROM Play songs as `.mid` files, as **Export .mid** does.
- `--reverb-settings d110emu-reverb.ini` renders with those reverb parameters, `--munt-reverb` with the MT-32 chip model; `--reverb-kit DIR` writes the reverb recording kit.

Run it with `--help` for everything.

## The project's files

The project files come from `premake5.lua`, and the sources don't include them. After adding or removing source files, or changing `premake5.lua`, run premake5 again (step 1 for your system). On Linux, `premake5 --os=macosx gmake` also makes the Mac's makefiles and `premake5 vs2022` the Visual Studio files, so one computer can make all three.

| Folder or file | Contents |
|---|---|
| `premake5.lua` | What premake5 makes the project files from: the programs, their sources and their settings on each system |
| `generate_vs2022.bat`, `tools\premake5.exe` | Makes the Visual Studio files on Windows, with the premake5 that comes with the sources |
| `src\` | The application: `AppCore.cpp` (the application without its interface: settings, ROMs, audio, MIDI, the memory, the unit's modes, the display's text), `App.cpp` (the Dear ImGui interface), `SynthEngine.cpp` (synth, threading, SysEx files, MIDI file player), `Mt32Translator.cpp` (MT-32 translation), `Mt32Presets.cpp` (MT-32 presets from its control ROM), `RomPlay.cpp` (ROM Play songs), `ReverbKit.cpp` and `ReverbSettingsFile.cpp` (the reverb recording kit and tuning file), `Lcd.cpp` (display), `MainWin32.cpp` and `Win32Host.cpp` (window and Direct3D 11), `MainSdl.cpp` and `SdlHost.cpp` (the window on Linux and macOS: SDL 3; `D110Emu.plist` is the Mac app's Info.plist), audio, MIDI input and output, settings. MT32Translator: `TranslatorCore.cpp` (the translator without its interface: settings, ports, presets, files), `TranslatorApp.cpp` (its window), `TranslatorTui.cpp` and `MainTranslatorTui.cpp` (its terminal version), `MidiPipe.cpp` (the translation pipe), `MainTranslatorWin32.cpp` and `MainTranslatorSdl.cpp` (`MT32Translator.plist` is the Mac app's Info.plist). The tone editor: `ToneEditor.cpp` (the editor, shared), `ToneModel.cpp` (the tone's parameters), `Piano.cpp`. ToneEditor: `ToneEditorApp.cpp` (UI), `UnitLink.cpp` (MIDI to and from the unit), `UnitSetup.cpp` (the unit's parts, rhythm setup and system area), `MainToneEditorWin32.cpp` and `MainToneEditorSdl.cpp` (`ToneEditor.plist`). PatternCapture: `PatternCaptureApp.cpp` (UI), `PatternCapture.cpp` (the takes), `MainPatternCaptureWin32.cpp`; D-20 patterns in `D20Rhythm.cpp`. The plugins: `PluginCore.cpp` (what both share: the App, the project state, the rendering), `PluginEditorWin32.cpp` (their window; `PluginEditorX11.cpp` on Linux, `PluginEditorMac.mm` on macOS, with `D110EmuVST3.plist` as the bundle's Info.plist), `AuPlugin.mm` (the Audio Unit; `D110EmuAU.plist` its Info.plist), `VstPlugin.cpp` and `Vst2.h` (VST2), `Vst3Plugin.cpp` (VST3), `MultiOutputConverter.cpp` (the part outputs, resampled with the mix). The terminal version: `TuiApp.cpp` (its interface), `TextScreen.cpp` (the screen, the terminal's escape sequences and keys), `TuiMenus.cpp` (the menus both terminal versions share), `TerminalPosix.cpp` and `TerminalWin32.cpp`, `MainTui.cpp` (with `TuiMain.h`, which MT32Translator's terminal version shares); MIDI on Linux in `MidiInputAlsa.cpp` and `MidiOutputAlsa.cpp` (with `MidiAlsa.h`), on macOS in `MidiInputCoreMidi.cpp` and `MidiOutputCoreMidi.cpp` (with `MidiCoreMidi.h`) |
| `res\` | The icon: `d110logo2ac.png` (the D-110 logo), and what `tools\make_icons.py` makes from it, `D110Emu.ico` (Windows, through `D110Emu.rc`) and `D110Emu.icns` (the Mac app). To change it, change the logo or the script and run it (it needs Python with Pillow and numpy); the builds only use the files it made |
| `mt32emu\` | Emulation core: Munt mt32emu 2.4.0 with the D-110 changes, and the D-series reverb model (`DSeriesReverb.cpp`) |
| `reverb-kit\` | MIDI files and instructions for recording a real unit's reverb (see the reverb in [README.md](README.md#d-110-features)) |
| `vendor\` | Dear ImGui 1.92.9b (with its Win32, Direct3D 11, SDL 3, OpenGL 3 and Metal backends), miniaudio 0.11.25, the VST3 SDK's interfaces (pluginterfaces 3.8.1), Apple's AudioUnitSDK 1.4.0 (unmodified) |
| `install-plugins-linux.sh`, `install-plugins-macos.sh` | Install the built audio plugins where DAWs look for them, with the ROMs (see step 3 for [Linux](#3-install-the-vst3-plugin) and [macOS](#3-install-the-vst3-plugin-and-the-audio-unit)) |
| `tools\` | The tools and tests' sources (see [Tests and tools](#tests-and-tools)), `mt32_waves.py` (generates the MT-32 translation's wave table from the ROMs), `reverb_analysis.py` (measures reverb recordings and fits the D-series reverb to them), `make_icons.py` (the icons in `res\`), `macos-app.sh` (makes the Mac apps and plugins whole after linking), `premake5.exe` |
