# D110Emu

A Roland D-110 emulator for Windows, Linux and macOS, built on Munt's mt32emu engine with D-110 ROM support. It uses a Dear ImGui interface and miniaudio for sound, and everything it needs is included, so on Windows Visual Studio is the only install. It also comes as VST2 and VST3 instruments for DAWs, with the same interface and an output for every part (see [VST2 plugin](#vst2-plugin) and [VST3 plugin](#vst3-plugin); on Linux and macOS, VST3), as an Audio Unit for Logic Pro and GarageBand on a Mac (see [Audio Unit on macOS](#audio-unit-on-macos)), and as a program with a text interface for a terminal, for computers without a screen, a Raspberry Pi as a sound module included (see [The terminal version](#the-terminal-version)). On Linux, see [Linux](#linux); on a Mac, [macOS](#macos).

The solution also builds three programs for real units:
- **MT32Translator**, a translation box: it takes MIDI meant for an MT-32 (from a game, a sequencer or an emulator) and passes it on to a real D-110, D-10 or D-20 (see [MT32Translator](#mt32translator)). It runs on Windows, Linux and macOS, and also comes as a terminal version, for a computer without a screen between the game and the unit.
- **ToneEditor**, a realtime tone editor for a real D-110, D-10, D-20 or MT-32, like Roland's PG-10 programmer (see [ToneEditor](#toneeditor)). D110Emu has the same editor in its **Tone** tab. It runs on Windows, Linux and macOS.
- **PatternCapture**, which records a D-20's preset rhythm patterns from what it plays on its MIDI OUT, for D110Emu's Patterns tab (see [PatternCapture](#patterncapture)). Windows only.

## Disclaimer

This project is in no way affiliated with or endorsed by Roland Corp.

## AI/LLM usage disclosure

This project has been created with the assistance of an AI/LLM.

## Building and starting it

[BUILDING.md](BUILDING.md) says how to build everything on Windows, Linux and macOS: what each system needs installed first, then premake5, which makes the project files, and the build itself. It also covers installing the plugins, the tests and tools, and the sources' layout.

On Windows the programs are then in `bin\Release`: `D110Emu.exe`, `D110EmuTUI.exe` (the terminal version), `MT32Translator.exe`, `MT32TranslatorTUI.exe` (its terminal version), `ToneEditor.exe` and `PatternCapture.exe`, with the VST2 plugin in `bin\Release\VST2\D110Emu.dll` and the VST3 plugin in the bundle `bin\Release\VST3\D110Emu.vst3`. Start D110Emu with Ctrl+F5 in Visual Studio, or double-click `D110Emu.exe`. They all need Windows 10 or 11 and link the C runtime statically, so an .exe can be copied to another PC along with a `roms` folder. `D110Emu.exe` and `D110EmuTUI.exe` carry the D-110 logo as their icon.

On Linux, see [Linux](#linux); on a Mac, [macOS](#macos). Before the first start, put the ROMs in place (see [ROMs](#roms)).

## Linux

D110Emu and its terminal version also run on Linux, a Raspberry Pi included: Raspberry Pi OS or Debian 13 and later, or any distribution with SDL 3. So do MT32Translator (with its terminal version) and ToneEditor. [BUILDING.md](BUILDING.md#linux) says how to build them; from the `d110emu` folder they then start as:

```sh
./bin/linux-Release/d110emu              # the window
./bin/linux-Release/d110emu-tui          # the terminal version
./bin/linux-Release/mt32translator       # MT32Translator (mt32translator-tui: its terminal version)
./bin/linux-Release/toneeditor           # ToneEditor
```

The VST3 plugin runs on Linux too (see [VST3 on Linux](#vst3-on-linux)). The programs find the `roms` folder next to `bin` as on Windows, and share their settings and memory in `~/.local/share/D110Emu` (see [Settings and memory](#settings-and-memory)).

- **The window** is the Windows one: the same menus, tabs and windows, on Wayland or X11, at the display's scale. Files open through the desktop's file dialog (**File > Open MIDI file...** and the rest); on a desktop without one, the status bar says so, and `sudo apt install zenity` gives it one. Files dropped on the window or named on the command line (`d110emu song.mid`) open too. The text is DejaVu Sans (else Noto Sans or Liberation Sans).
- **Sound**: miniaudio plays through PipeWire or PulseAudio where they run, else straight through ALSA. **File > Configuration...** (the terminal version: **O** > **Device**, or `--list` and `--audio-device`) chooses the output: on a Raspberry Pi 5, which has no headphone socket, HDMI, a USB audio interface or a DAC board. The **Buffer** trades latency against dropouts: smaller is quicker, larger if the sound crackles. The emulation takes about 5% of one of a Pi 5's cores.
- **MIDI** comes through the ALSA sequencer, which the programs load when they run. The configuration window (the terminal version: **O** > **MIDI inputs**) lists the MIDI interfaces (USB ones, and any other the kernel has a driver for) and other programs' output ports by name, as `aconnect -l` shows them: ticked ones are remembered and connected again whenever they are plugged in. Programs can also send to D110Emu themselves: it is the sequencer client **D110Emu** with the port **MIDI In** (`aplaymidi -p D110Emu song.mid`, or `aconnect` from a port to it). MT32Translator and ToneEditor send through the sequencer too: their outputs are the MIDI interfaces and other programs' input ports, and they are the clients **MT32Translator** and **ToneEditor**.
- **Permissions**: the sound devices and the sequencer belong to the `audio` group, which Raspberry Pi OS's first user is in. For another user, `sudo usermod -aG audio USER`, then log in again; without it, the MIDI input says the sequencer cannot be opened.

## macOS

D110Emu, its terminal version, the VST3 plugin (see [VST3 on macOS](#vst3-on-macos)) and the Audio Unit (see [Audio Unit on macOS](#audio-unit-on-macos)) run on a Mac with macOS 11 or later, and so do MT32Translator (with its terminal version) and ToneEditor. [BUILDING.md](BUILDING.md#macos) says how to build them; in Terminal, from the `d110emu` folder, they then start as:

```sh
open bin/macosx-Release/D110Emu.app     # the window (or double-click it in Finder)
./bin/macosx-Release/d110emu-tui        # the terminal version
open bin/macosx-Release/MT32Translator.app    # also ToneEditor.app, and ./bin/macosx-Release/mt32translator-tui
```

`D110Emu.app` carries SDL 3's framework inside it, so it needs nothing installed: it runs on any Mac of its kind (built on Apple silicon, it runs on Apple silicon) with macOS 11 or later. The programs find the `roms` folder next to `bin`, as on the other systems, and share their settings and memory in `~/Library/Application Support/D110Emu` (see [Settings and memory](#settings-and-memory)).

- **The window** is the Windows one: the same menus, tabs and windows, sharp on a Retina display. Files open through the Mac's own file dialogs; MIDI and SysEx files dropped on the window or on its Dock icon, or opened with **Open With** in Finder, open too. What is Ctrl on the other systems is Cmd here (Cmd+O opens a MIDI file), Ctrl+click is a right-click, and Cmd+Q quits, saving the memory as **File > Exit** does. The text is Helvetica.
- **The icon**: `D110Emu.app` carries the D-110 logo, drawn on Apple's rounded-square template. Finder shows it on the app wherever it is, but the Dock may show the plain app icon while the app runs from `bin/macosx-Release`: it shows its own once the app is in Applications (drag it there in Finder, or `ditto bin/macosx-Release/D110Emu.app /Applications/D110Emu.app`). Each build also registers the app with macOS again, as Xcode does.
- **Other Macs**: copy `D110Emu.app` (in a zip, say). There, the ROMs go in `~/Library/Application Support/D110Emu/roms` or a `roms` folder next to the app, or **File > Configuration...** chooses them. The app also looks inside itself, in `D110Emu.app/Contents/Resources/roms`: a `roms` folder put there changes the signed app, so sign it again afterwards (`codesign --force --sign - D110Emu.app`), or macOS refuses a downloaded copy as damaged. The app is signed ad hoc, not by an Apple developer ID, so the first time it opens on a Mac that downloaded it, macOS asks: Control-click it and choose **Open**, or on macOS 15 and later, **System Settings > Privacy & Security > Open Anyway**.
- **Sound** goes through Core Audio. **File > Configuration...** (the terminal version: **O** > **Device**, or `--list` and `--audio-device`) chooses the output; the system's default is the one the Mac's sound settings choose.
- **MIDI** comes through CoreMIDI. The configuration window (the terminal version: **O** > **MIDI inputs**, or `--midi-in`) lists the Mac's MIDI sources by name: USB interfaces, Bluetooth and network MIDI set up in Audio MIDI Setup, the IAC Driver's buses and other programs' virtual outputs. Ticked ones are remembered and connected again whenever they come back. Programs can also send to D110Emu themselves: while it runs, it is a MIDI destination named **D110Emu**, which a DAW or a game lists among its MIDI outputs. MT32Translator and ToneEditor send to the Mac's MIDI destinations the same way, and are destinations of their own (**MT32Translator**, **ToneEditor**).
- **Terminal**: in Terminal and iTerm2 the terminal version draws with Unicode line characters and 256 colours, or 24-bit colours where the terminal says it has them (`COLORTERM=truecolor`, as iTerm2 sets it).

## ROMs

D110Emu runs the D-110's own firmware and samples, so it needs images of the D-110's ROM chips: a **Control ROM** and a **PCM ROM**, each joined from two chip dumps. Files are recognised by their content (SHA1), not by their names.

### The chips you need

| Chip | Holds | Size | Dump file (as MAME names it) | SHA1 of the dump |
|---|---|---|---|---|
| IC19 | Firmware v1.10 | 32 KiB (32,768 bytes) | `d-110.v1.10.ic19.bin` | `28635510f30d6c1fb88e00da03e5b4e045c380cb` |
| IC12 | R15179873 (LH5310-97): preset and rhythm tones, the sample table, the ROM Play songs | 128 KiB (131,072 bytes) | `r15179873-lh5310-97.ic12.bin` | `05587a0542b01625dcde37de5bb339880e47eb93` |
| IC8 | R15179880: samples, first half | 512 KiB (524,288 bytes) | `r15179880.ic8.bin` | `9c59f50518a070461b2ec6cb4e43ee7cc1e905b6` |
| IC7 | R15179878: samples, second half | 512 KiB (524,288 bytes) | `r15179878.ic7.bin` | `6760d14900161b8715c2bfd4ebe997877087c90c` |

Not needed: IC6 (`r15179879.ic6.bin`, 32 KiB), which D110Emu does not use, and firmware v1.06 (`d-110.v1.06.ic19.bin`), which it does not recognise.

### Joining them

The Control ROM is IC19 followed by IC12, and the PCM ROM is IC8 followed by IC7: one file after the other, byte for byte, in that order. In a Command Prompt, in the folder that holds the dumps (in PowerShell, put `cmd /c` in front of each line):

```bat
copy /b d-110.v1.10.ic19.bin + r15179873-lh5310-97.ic12.bin CONTROLromcombo.bin
copy /b r15179880.ic8.bin + r15179878.ic7.bin PCMromcombo.bin
```

On Linux or macOS:

```sh
cat d-110.v1.10.ic19.bin r15179873-lh5310-97.ic12.bin > CONTROLromcombo.bin
cat r15179880.ic8.bin r15179878.ic7.bin > PCMromcombo.bin
```

### Checking them

| Image | Size | SHA1 |
|---|---|---|
| `CONTROLromcombo.bin` (D-110 Control v1.10) | 163,840 bytes | `8d549f3382a23b8faa64e3988f913d403a92887f` |
| `PCMromcombo.bin` (D-110 PCM ROM) | 1,048,576 bytes | `8eb2e3857a36272eb66d64d8dcb82a6b14c8d26e` |

`certutil -hashfile CONTROLromcombo.bin SHA1` in a Command Prompt shows a file's SHA1 (`Get-FileHash -Algorithm SHA1 CONTROLromcombo.bin` in PowerShell, `sha1sum` on Linux, `shasum` on macOS). Chips joined in the other order, or another firmware version, give another SHA1, and D110Emu then does not list the file. It also accepts one other v1.10 Control image, from a different dump: SHA1 `8b064509db7520e0633b8bea9e3ce3945fc7de54`.

### Where they go

At startup the emulator looks for a folder named `roms`: inside the Mac app (`D110Emu.app/Contents/Resources`), next to the .exe (or a plugin) and in each parent folder, in the working folder, and in D110Emu's own folder (see [Settings and memory](#settings-and-memory)). So a `roms` folder next to `bin\` is found when the program runs from `bin\`, and one in `%APPDATA%\D110Emu` (`~/Library/Application Support/D110Emu` on a Mac, `~/.local/share/D110Emu` on Linux) wherever the program is. **File > Configuration...** chooses another folder, and the Control and PCM ROM when the folder holds more than one.

## Settings and memory

D110Emu keeps its settings (`d110emu.ini`), the memory (`d110emu-memory.syx`) and the reverb tuning (`d110emu-reverb.ini`) in a folder of its own in your user folder. The window and the terminal version share them, the plugins keep their computer-wide settings there (`d110emu-vst.ini`), and so do MT32Translator, ToneEditor and PatternCapture:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\D110Emu` (the roaming AppData) |
| macOS | `~/Library/Application Support/D110Emu` |
| Linux | `$XDG_DATA_HOME/D110Emu`, else `~/.local/share/D110Emu` |

MT32Translator's window and its terminal version share their settings (`mt32translator.ini`) as D110Emu's two do. **Help > About** shows the folder, and a `roms` folder in it is found too. Earlier versions kept these files next to the program (or the plugin): the first time a program of this version starts, it copies its own files from there into this folder, and says so in the log. The old files stay where they were, unused.

### Optional files

| File | What it adds |
|---|---|
| An MT-32 or CM-32L Control ROM (e.g. `MT32_CONTROL.ROM`) | MT-32 translation plays the MT-32's own presets, and tone banks d and e hold them |
| An MT-32 or CM-32L PCM ROM (e.g. `MT32_PCM.ROM`) | With a Control ROM of the same model, the emulator runs as that unit, for comparisons |
| `D-20 preset patterns.syx` | The D-20's 32 preset rhythm patterns for the Patterns tab, recorded from a D-20 with [PatternCapture](#patterncapture); found by this name |
| `D-20 initial memory.syx` | A bulk dump of a D-20 after a factory reset and a power cycle: the programmable patterns P-51–P-88 start with its patterns, as a D-20's do; found by this name |

The MT-32 and CM-32L ROMs are the whole images other Munt-based emulators use: Control ROM MT-32 v1.04–v1.07, BlueRidge or v2.04, or CM-32L/LAPC-I v1.00 or v1.02; PCM ROM MT-32, or CM-32L/CM-64/LAPC-I. D110Emu does not join their chip halves. The D-110 features below apply to D-110 ROMs only.

## The window

The main window shows the unit: its display and the MIDI file player, then the tabs. The settings are tucked away:
- **File > Configuration...** holds the ROMs, the audio output and the MIDI inputs. It opens by itself when the emulator can't make a sound (no ROMs, no audio device), and the status bar at the bottom names such a problem in red until it is solved; click it for the window.
- The **System** tab, after Patterns, holds the sound (master tune, reverb, output and reverb gain, emulation quirks), MIDI (MIDI extensions, MT-32 translation) and the system (control channel, unit number, partials, 16 parts, **Restart synth**).
- **View > Window size** makes the window and its text 75–200% of the size the display's scaling gives, on top of that scaling (Windows' display settings, the Linux desktop's, or the Mac's display resolution). D110Emu opens at the chosen size next time (`window_zoom` in `d110emu.ini`), and the window can still be resized by dragging.

## Playing

- **From another program** (DOSBox, ScummVM, a DAW): create a virtual MIDI port with [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html), set the program's MIDI output to that port, and tick the port under **MIDI input** in **File > Configuration...**. Hardware MIDI interfaces are listed there too.
- **MIDI files**: use **Open MIDI...** (Ctrl+O), drag a `.mid` onto the window, or pass it on the command line.
- **Keyboard**: click the on-screen piano, or use the computer keyboard. The bottom and top letter rows (`Z`–`/` and `Q`–`P` on a US keyboard) play two octaves, and Page Up/Down shifts them. The keys go by position, so QWERTZ and AZERTY keyboards get the same layout. Notes go to the channel set above the piano.
  - The on-screen keyboard sits under the **Play** and **Performance** tabs. A tab that needs more room than the window has scrolls on its own, between the LCD and player at the top (which stay in view) and the keyboard. **View > Keyboard** hides it, and the Tone tab's audition keys with it (their Play, Hold and Repeat controls stay); the computer keyboard plays either way. In the plugins it starts hidden.
  - In the Performance tab, in performance mode, the keyboard (and the computer keyboard) plays the performance channel, so the patch being edited is heard whatever the keyboard's own channel is; its program change then selects a performance.
- **MIDI options**:
  - **Reset MIDI** (the System tab's MIDI section, and under **Synth**) stops all sound and resets every part's controllers, pedal and pitch bend, and what the MIDI extensions set (the bend ranges go back to 2 semitones). Every part's level goes to 100 and its pan to the centre, as a GM or GS module resets them; timbres, channels and the rhythm keys' own levels and pans stay. With MT-32 ROMs, levels and pans stay as the MT-32 has them.
  - **MIDI cable speed** (File > Configuration) spaces messages the way a MIDI cable delivers them to a real unit (about 0.8 ms per note). It is off by default, so dense music from a DAW plays on time.
  - **MIDI extensions** (System tab, on by default) understand more than the real unit, and make it behave more like a General MIDI or GS module:
    - The pitch bend range belongs to the MIDI channel, as on GM modules: RPN 0 sets it (up to 24 semitones, and CC 38 adds cents), program changes and timbre edits keep it, and it is 2 semitones at the start and after a reset, whatever the timbre says. On the unit, and without the extensions, it is the timbre's bender range, which RPN 0 changes until the next program change. The Parts tab shows the range a part bends by under the timbre's.
    - GS NRPNs and GM2 sound controllers change a part's sound, relative to its timbre (64, or 40H, is no change): the filter cutoff (CC 74, NRPN 01H 20H) while it plays and its resonance (CC 71, 01H 21H); the envelopes' attack time (CC 73, 01H 63H), decay time (CC 75, 01H 64H) and release time (CC 72, 01H 66H), from the next envelope segment (each step of 8 doubles or halves the time); and the vibrato's rate (CC 76, 01H 08H; 16 steps double or halve it) and depth (CC 77, 01H 09H). The D-110's vibrato has no delay, so NRPN 01H 0AH does nothing. Reset All Controllers keeps these values, as on GM and GS modules.
    - Portamento, as GS modules have it: with CC 65 on, each note glides to its pitch from the last note played on its channel, while earlier notes go on sounding. CC 5 sets the speed, from an octave in 0.04 s (0) through 0.6 s (64) to 9 s (127); a wider interval takes longer (TiMidity++'s curve for GS music). CC 84 names the key the next note glides from, with portamento on or off, for that note only; if a note still sounds on that key, it moves to the new key instead of a new note starting (legato), and the old key's note-off no longer ends it. Only the pitch glides, and sounds that do not follow the keyboard (key follow 0) stay put. Reset All Controllers switches portamento off. The rhythm part has none.
    - RPN 1 and 2 set a part's fine tune and key shift.
    - GS master tune (`F0 41 10 42 12 40 00 00` and four nibbles, as the SC-55 and SC-88 take it) tunes the whole synth by −100 to +100 cents, on top of its own master tune, which stays as it is in memory. The System tab shows it under the Master tune slider, with the A4 it makes.
    - GM, GS and XG reset messages reset the MIDI channels (and everything above), put every part's level at 100 and its pan at the centre, and end a GS master tune, as Reset MIDI does.
    - After a GM, GS or XG reset, SysEx for the MT-32's address map no longer changes the part channels: for the rest of that MIDI file, or in live MIDI until the synth restarts (or the extensions or MT-32 translation are switched). Files made for a GS module often carry such SysEx for an MT-32 beside it, to switch the MT-32's parts off, which would silence the D-110 too. The rest of such a message is taken, and the log says it once. The next file, and music without a GM reset, set the channels as on the unit.
    - The extensions are off, whatever the checkbox says, while music made for the real units plays: in MT-32 translation, with the MT-32's own ROMs, and while a ROM Play song plays or pauses. The System tab says so next to the checkbox (the terminal version's Options too), and they come back when that ends.
  - **MT-32 translation** (System tab) plays music made for the MT-32 or CM-32L, such as game soundtracks. It works like a translation box between the MT-32 program and a D-110: MIDI input, MIDI files and SysEx files are converted.
    - The MT-32's PCM waves become the D-110 samples of the same instruments, retuned to the MT-32's pitch. Many are the very same recordings (the organ, violin, flutes, tubular bells and more), which then play exactly as on the MT-32. The Orchestra Hit, which the D-series lacks, becomes Violin-2's attack.
    - **MT-32 presets** (under the checkbox, when an MT-32 or CM-32L control ROM is in the ROM folder):
      - **The MT-32's own** (the default): the MT-32's preset timbres and rhythm sounds play, translated from its control ROM.
      - **D-110 stand-ins**: D-110 presets of the same kind stand in, as on a real D-110 (also what plays without the ROM). They were matched by comparing the tones' data and by rendering both at the same key, then checked by ear on a real D-20: Syn Brass 1 becomes the D-110's (much richer) Brass 1, Syn Brass 4 becomes Steam Pad, Doctor Solo becomes VibeString, Schooldaze becomes Space Horn, Syn Brass 3 becomes Syn Lead 2; presets the D-110 has an octave or two away get a key shift (the basses, Elec Piano 2, the flutes, the vibes, the Triangle and more).
      - **Chosen one by one**: each MT-32 preset and rhythm sound plays the MT-32's own or a D-110 stand-in, as you choose.
      - **Choose...** opens the **MT-32 presets** window (also View > MT-32 presets): a Presets and a Rhythm tab list what each MT-32 preset (A1–B64) and rhythm sound (R1–R30) plays. Pick the D-110's preset or rhythm tone and its octave, and in *Chosen one by one* whether the MT-32's own plays; **All built in** brings back the choices that come with the program. The choices are the same as MT32Translator's (`preset_choices` in the .ini files, which can be copied between them).
      - While translating with the MT-32's own or chosen presets, they take the place of the D-110's preset tones (a11–b88 are A1–B64, r01–r63 are R1–R63), and the D-110's own a and b move to tone banks **d** and **e**. Everything goes back when translation ends.
    - **Tone banks d and e**: with the MT-32's control ROM, d11–e88 hold the MT-32's presets A1–B64, translated for the D-110, at any time (and the D-110's a and b while translating as above). Any part can play them: choose them in a tone picker (Parts or Tone tab), or choose the timbres **P-D11**–**P-E88** (Play or Parts tab), which play them without key shift or fine tune, with bender range 12. Patches and timbres keep them; without the ROM such parts play a and b of the same number.
    - **Roomy toms**: the MT-32 plays its toms with one sample, the one the D-110's TomTom2 set uses at the same pitches, so MT-32 toms play that set. This option plays the roomier TomTom1 set instead.
    - Pans are mirrored (MT-32 panpot 0 is right, D-110 0 is left), and reverb modes are mapped.
    - SysEx for the MT-32's unit (17) arrives whatever the D-110's unit number is.
    - SysEx master volume works, and an MT-32 reset sets up the MT-32's power-on state.
    - Turning it on sets up that power-on state: parts 1–8 on channels 2–9, rhythm on 10, and the MT-32's patches, rhythm setup and partial reserves. **MT-32** shows next to the display while it is on.
    - Turning it off brings back the D-110 tones, timbres, rhythm setup and parts as they were. The battery memory keeps the D-110 setup.
    - D-110 and D-20 SysEx files (which use areas the MT-32 lacks) load untranslated. D-110 patches and the card are not touched.
    - The CM-32L's sound effects (bank-2 samples, rhythm keys for R31–R63) have no D-110 counterpart; they are silent or become noise.
- **Channels**: like the real D-110, Parts 1–8 listen on MIDI channels 1–8 and the rhythm part on channel 10 after initialization. Change a part's channel in the parts table, or pick **Synth > MT-32 channels** (2–9) for MT-32 game music (**MT-32 translation** does this too). Recalling a patch sets the channels stored in it, as on the real unit.

## D-110 features

- **Display**: a 16×2 LCD at the top shows the play mode like the unit ("12345678R  Part1" / "I-B15:SlapBass 1"). Part numbers darken while they sound. Its colours (the D-110's lime green, the D-10/D-20's bright yellow-green on black, yellow-green, blue, amber, unlit grey) are under View > LCD colours, or right-click it. **Custom** there uses your own colours, and **Edit custom colours...** sets them: choose the glass, the unlit dots or the lit dots and pick its colour, and the display changes as you drag. **Start from a preset** copies a preset's colours to start from, and **Undo changes** goes back to the colours from when the window opened. They are saved in the settings file as `#RRGGBB` values (`lcd_custom_glass`, `lcd_custom_dot_off`, `lcd_custom_dot_on`, with `lcd_scheme = custom`). Use the arrows to choose the part, and **Patch view** to show the patch. In D-20 performance mode it shows the patch as a D-20 does ("I-A11 SPLIT C4": the key mode, and the split key in Split mode, over the patch name), and the ▲▼ buttons below it, like the D-20's DISPLAY buttons, switch to the patch's upper and lower tones ("U:a61 Clarinet 2" / "L:a60 Clarinet 1"). Text sent over SysEx is shown until a display reset or a click. Its characters are the units' Japanese LCD's: a backslash in a name or a message shows as a yen sign (¥), as on the unit. The patch and the performance patch on the display are remembered for the next start.
- **Play tab**: a row per part with its channel, timbre and tone (click to choose another timbre), level, pan, output (a menu), MIDI volume and expression, the partials it plays against its reserve, and the keys it plays. Below the parts, the partial display shows what each of the synth's partials is doing (attack, sustain, release); **View > Partials** hides it.
  - **M** mutes a part and **S** solos it (only soloed parts are heard); both act at once, also on notes already sounding. Right-click M to unmute every part, right-click S to solo that part alone (or to end the solo).
  - The value cells work like the unit's VALUE buttons: left click steps up (pan: to the right), right click steps down. Both repeat while held, the mouse wheel steps too, and Shift makes steps of 10.
  - Volume and expression send CC 7 and CC 11 on the part's channel.
- **Memory is kept**, like the unit's battery-backed RAM: tones, timbres, patches, the rhythm setup, the system area and the parts' current settings are saved to `d110emu-memory.syx` (see [Settings and memory](#settings-and-memory)) on exit, and loaded at start. The file is a standard SysEx bulk dump that a real D-110 accepts too. **File > Initialize memory** starts over.
- **SysEx files**: **File > Load SysEx file...** (or drop a `.syx` on the window) applies a dump immediately. **File > Save memory as SysEx...** writes the memory as a bulk dump.
  - D-20 dumps load correctly: tones, timbres, performance patches, rhythm setup, reverb, partial reserves, and part levels and pans are all taken over.
  - The D-20's placeholders for the D-110's part channels are ignored.
  - Its rhythm patterns and rhythm track are kept for the **Patterns** tab.
- **Parts tab**: per part, choose a timbre from memory (A11–B88) or a tone directly (a, b, i, r groups), and set level, pan, output (Mix, Mix + reverb, Multi 1–6), key shift, fine tune, bender range, assign mode, key range and partial reserve. **Write timbre to** stores the part's settings in timbre memory.
- **Multi outputs**: the D-110 has six mono MULTI outputs besides its stereo mix, and a part (or, in the Rhythm tab, a rhythm key) goes to one with its output **Multi 1–6**. In stereo they are mixed in, dry and centred, and Multi 5 and 6 are silent while the reverb is on, as on the unit. To hear them apart:
  - **7.1 surround**: set **Channels** to **7.1 surround** under **Audio output** in **File > Configuration...**. Multi 1 and 2 play at the front left and right, with the mix; Multi 3 and 4 at the rear (back) left and right; Multi 5 and 6 at the side left and right. The centre and LFE speakers stay silent. Each Multi output plays at its full level on its speaker, dry, and Multi 5 and 6 play even with the reverb on. Set the device to 7.1 speakers in Windows (the Sound control panel, `mmsys.cpl`: select the device, **Configure**, **7.1 Surround**). On a device set to fewer speakers, the speakers it lacks are mixed into those it has, and the configuration window says so under the device's details.
  - **Multi**, under **Channels**, chooses how the 7.1 speakers play them: **Six mono outputs**, each on a speaker of its own as above, as the unit's mono jacks; or **Three stereo pairs**: **Multi 1+2** at the front, **Multi 3+4** at the rear and **Multi 5+6** at the side, where a part or rhythm key on either output of a pair plays in stereo with its pan (say strings on the rear pair). The Output menus then offer the three pairs instead of Multi 1-6. The terminal version has the same choice in its options.
  - **The plugins** have the Multi outputs as outputs of their own, six mono ones or three stereo pairs (see [VST2 plugin](#vst2-plugin)).
- **Tone tab**: a realtime tone editor for the part the Parts tab edits, like Roland's PG-10 programmer. Every change goes straight to the part's tone temporary area (as the SysEx a programmer sends), and the part plays it from the next note.
  - **All of the tone's parameters**:
    - the name, structures 1-2 and 3-4 (with a diagram: S synthesizer, P PCM, R ring modulator) and the envelope mode;
    - the four partials side by side, grouped as on the unit: WG, pitch envelope, pitch LFO, TVF, TVF envelope, TVA, TVA envelope. What doesn't apply to a PCM partial (waveform, pulse width, the filter) is dimmed;
    - right-click a value to set all four partials to it; Ctrl+wheel steps a value.
  - **Envelope graphs**: the pitch, TVF and TVA envelopes of all four partials, in the columns' colours. Drag a point sideways for its time, up or down for its level.
  - **Partials**: the tick switches a partial on or off, **S** plays it alone (solo), and **...** copies, pastes, swaps or initialises it.
  - **Undo** and **Redo** (Ctrl+Z, Ctrl+Y), **Compare** (plays the tone as it was loaded until clicked again), **Init**.
  - **Audition**: the editor's keyboard, the computer keys and MIDI input on the part's channel play the part.
    - **Play** plays the audition note once, **Hold** holds it, and **Repeat** plays it again and again. Right-click a key to choose the audition note.
    - **Restrike** plays held notes again after each change, because the unit applies a change from the next note on.
  - **Tone** loads another tone into the part (Undo brings the edit back).
  - **Write to** stores the edited tone in i11–i88 (or the card's c11–c88) with the unit's tone write, and points the part at it.
  - **Load...** takes a tone from a SysEx file: a single tone, a bulk dump, or MT-32 timbres, whose waves it can translate. **Save...** writes the tone as a SysEx file that any LA synth loads (part 1's tone temporary area).
  - With MT-32 ROMs it edits the MT-32's timbres, with the MT-32's wave names.
- **Timbres tab**: timbre memory A11–B88 (and the card's C-A11–C-B88) as a table, with the tone, key shift, fine tune, bender range, assign mode and output of each.
  - Click a timbre's number to play it on the edited part. A part that plays a timbre takes its changes at once.
  - Right-click a number to copy and paste timbres.
- **Patches tab**: click one of the 64 patches (I-11 to I-88) to recall it, rename the current patch, and **Write** the current setup as a patch. With **Control channel** (System tab) set, program changes on that channel recall patches, as on the unit.
- **Rhythm tab**: tone, level, pan and output for every key of the rhythm part.
- **Patterns tab**: the D-20's rhythm machine, for the rhythm patterns and rhythm track of D-20 dumps.
  - **Play pattern** loops the selected pattern; double-clicking a pattern does the same. While a pattern plays, clicking another one plays it from the next bar's first beat, as on the D-20: it shows in amber, with **Next** by the tempo, until then (click the playing pattern to stay on it). **Play track** plays the rhythm track bar by bar. Both play on the rhythm part with the current rhythm setup (a D-20 dump brings its own). **Tempo** applies at once; D-20 dumps hold no tempo.
  - The step view shows the pattern (or the one playing) with its drum sounds and a playhead. The track view shows every bar and highlights the one playing.
  - While the tab is shown, the display shows the D-20's Pattern Play screen: "Pattern Play" over the selected pattern and its name ("P-11:8Beat 1", or "P-51:UserPattern" for a programmable one). A pattern waiting for the next bar blinks its number, as on the D-20 (at 111/60 Hz, about 1.85 times a second).
  - P-11–P-48 are the D-20's preset patterns. They are in its ROM, so no dump holds them. D110Emu loads them at every start from `D-20 preset patterns.syx` in the `roms` folder, recorded from a D-20 with [PatternCapture](#patterncapture). **Load presets P-11–P-48...** loads another dump whose P-51–P-88 hold them, until the next start. Without them, the track plays preset bars as 4/4 rests.
  - P-51–P-88 are the programmable patterns. A D-20 starts them all with the same simple 4/4 beat after a factory reset, and so does D110Emu, from `D-20 initial memory.syx` in the `roms` folder (a D-20 dumped then): it fills every slot the memory has never held, so patterns you loaded stay. A D-20 dump brings its own.
  - The rhythm track shows each bar's pattern number, and blank bars as their length ("4/4"). It starts as the D-20's factory track, the preset patterns P-11–P-48 in order, each played twice; a D-20 dump brings its own, and **Factory track...** brings the factory one back. **Play track** plays it through once and stops, like the D-20.
  - **Export pattern .mid** and **Export track .mid** write Standard MIDI Files at the chosen tempo.
  - The patterns and track are kept in the memory file.
  - The D-20 can't send its sequencer tracks over MIDI (only to disk). To bring a sequence over, mute its tracks on the D-20: muted tracks play out of MIDI OUT on their parts' channels. Record that in a sequencer and open the MIDI file here, with the D-20's sound data loaded.
- **ROM Play**: the eight demo songs from the control ROM, one at a time or as a chain, like the unit's ROM Play mode:
  - The songs play at the unit's own speed, and at 442 Hz as the unit plays them: they were made with pre-release firmware, whose default pitch that was. Your master tune comes back when the song stops.
  - Bumble Dee's data ends with 19 seconds of silence; here it ends 3.5 seconds after its last note, as the other songs do.
  - They use their own timbres, including nine tones that exist only for the demos. For example, Macho Memory's A34 plays "Syn Lead 1", an edited b31.
  - Each song has its own reverb, rhythm setup and partial reserves.
  - Your channels, rhythm setup and parts are restored when the song stops.

  **Export .mid** saves a song, or all of them, as Standard MIDI Files:
  - Each file carries the song's tempo (for example 168.6 BPM for Macho Memory) and starts on a bar line.
  - A spaced-out SysEx setup at the start puts a real D-110 into the ROM Play state, so it can play the file from a sequencer.
  - The songs send no note-offs to the drums (the D-110's drum tones ignore them), so the files add short ones for other synths and DAWs.
- **System** (System tab): control channel, unit number (SysEx device), and **Partials** for more polyphony than the real unit's 32. More partials restart the synth, but memory is kept. The partial reserves keep counting out of 32 and scale with the partial count: with 512 partials, a reserve of 4 keeps about 64 partials for its part. When there are at least 4 partials per part, every part keeps at least 4, even with a reserve of 0 (the others give up a little of their share for it). **Restart synth** power-cycles the emulated synth; with the D-110 ROMs its memory and parts are kept, with every part's level at 100 and its pan centred, as Reset MIDI leaves them (a restart for a setting, such as more partials, keeps them as they were).
- **16 parts** (System tab): 15 melodic parts and rhythm, like a 16-channel sound module.
  - Parts 9–15 start on MIDI channels 9 and 11–16, so every channel has a part and channel 10 stays with the rhythm part.
  - Use 512 partials to go with them. The partial reserves of all parts then share out the pool in proportion.
  - Their settings travel in SysEx at 13 00 00 (part temporary areas), 14 00 00 (tones) and 11 00 00 (reserves at 00–06, channels at 08–0E), and in the memory file. D-110 patches store parts 1–8 only.
- **Performance tab**: the D-20's performance mode.
  - One performance patch plays on one MIDI channel: part 1 takes its upper tone and part 2 its lower tone, as **Whole** (upper tone only), **Dual** (layered, weighted by the U/L balance) or **Split** (lower tone below the split point).
  - The patch also sets tuning, bender, assign mode and reverb per tone, the patch level and the reverb.
  - Program changes on the performance channel select the 128 performance patches (A11–B88). They start as a D-20's do after a factory reset: "<Initial Patch>", split at C4, with the tones in order (A11 plays a01 above the split and b01 below it, up to A88's a64 and b64; bank B the other way round, from B11's b01 above a01 to B88's b64 above a64). Load a D-20 dump (such as the D-20's factory data) for real patches.
  - Edit the current performance on the tab and **Write** it to patch memory.
  - While the mode is on, the other parts are silent (the rhythm part keeps playing), and turning it off brings the multi-timbral setup back. ROM Play turns the mode off.
- **Memory card** (**File > Memory card**): an M-256D RAM card with 64 more tones (c11–c88), 128 timbres (C-A11–C-B88) and 64 patches (C-11–C-88).
  - **New card...** creates a blank card file, and **Insert card...** opens an existing one.
  - The card file is kept up to date on eject and on exit, and it is inserted again at the next start.
  - With a card inserted:
    - the tone picker has a **c (card)** group;
    - timbre lists and **Write timbre to** include C-A11–C-B88;
    - the Patches tab shows the card's patches;
    - program changes 64–127 on the control channel recall card patches.
  - **Save memory to card** and **Load memory from card** copy all tones, timbres and patches between the unit and the card, like the unit's Save/Load Card functions.
  - SysEx reaches the card at 18 00 00 (tones), 15 00 00 (timbres) and 16 00 00 (patches), an extension of the unit's address map. The card file is a dump of those areas.
- **Reverb** (System tab): the D-110's eight types as its manual names them (Small Room, Medium Room, Medium Hall, Large Hall, Plate, Delay 1–3, and Off), with Reverb Time 1–8 and Reverb Level 0–7.
  - **Reverb model**: **D-series** (the default) plays all eight types through a model of the D-series' reverb. **MT-32 chip** is mt32emu's emulation of the MT-32 family's reverb as before: it has four types (the rooms play its Room, the halls its Hall, the delays its Tap delay) and rings far longer than a D-series unit.
  - The D-series model is fitted to recordings of a real D-20 made with the kit below: for every type and Reverb Time, its level against the dry sound, its decay in each octave band, its tone and when it starts, and for the delays their echoes (Delay 1 a single echo, Delay 2 a repeating one, Delay 3 a left and right echo repeating from the right). Its decay runs from about 0.2 s (Small Room, Reverb Time 1) to 2.2 s (Plate, Reverb Time 8).
  - **Tune...** (or **View > Reverb tuning**) shows its parameters for each type, applied as you move them: the wet level for each Reverb Level, the decay (RT60) for each Reverb Time, damping, pre-delay, bandwidth, diffusion and size, and for the delays their left and right taps and feedback. **Test chord** plays a chord through it. They are kept in `d110emu-reverb.ini` (see [Settings and memory](#settings-and-memory)).
  - **Measuring a real unit**: `reverb-kit/` holds MIDI files that play short bursts through every reverb setting of a real D-110 or D-10/D-20, and `README.txt` there says how to record them. From such a recording, `tools/reverb_analysis.py` measures the unit's reverb (level, pre-delay, decay per frequency band, spectrum, delay echoes) and fits the model's parameters to it. A recording started by hand works: the bursts are found wherever they are, and the player's and recorder's clocks may differ.
- **Master tune** (System tab): the unit's tuning, A4 from 427.5 to 452.7 Hz, 440.0 Hz by default, as the D-110 and D-10/D-20 leave the factory (their pre-release firmware had 442.0 Hz); MT-32 translation sets the MT-32's 442.0 Hz. Right-click the slider to go back to the default. It lives in the unit's memory, as on the real unit, so SysEx can set it too and the memory file keeps it.
- **Drums**: as on the unit, the closed hi-hats (r01, r02) cut off Open High Hat-1 (r03), whichever keys they are on. Open High Hat-2 (r04) rings on.
- **Assign mode**: timbres and parts start in POLY 3 (a repeated key sounds again without cutting off the previous note), the D-110's own default. Memory saved by earlier versions keeps the single-note mode it was created with; **File > Initialize memory** switches it over.
- Pan follows the D-110: panpot 0 and MIDI pan 0 are left (the MT-32 is the other way round). With **Nice panning** (System tab > Emulation quirks), pan becomes a slider from −64 to +64 for parts (Play and Parts tabs) and rhythm keys (Rhythm tab), in 129 steps instead of the unit's 15. Drag it, use the mouse wheel (Shift: 10 at a time), double-click for the centre or Ctrl+click to type. MIDI pan (CC 10) then keeps its full resolution. The fine pan is kept in the memory file (an extension at 12 00 00 and 12 01 00). Writing the ordinary panpot drops it. MIDI volume (CC 7) scales a part together with its level, instead of replacing the level as on the MT-32.

Settings are saved to `d110emu.ini` (see [Settings and memory](#settings-and-memory)).

## VST2 plugin

`D110Emu.dll` is the emulator as a VST 2.4 instrument, for DAWs on 64-bit Windows. It has the whole interface: the display, all the tabs, the editors and the windows under View. The host plays MIDI into it (notes, controllers and SysEx, each at its own sample), and its outputs go to the host's mixer: the mix, an output for every part, and the D-110's six Multi outputs.

- **Installing**: [BUILDING.md](BUILDING.md#3-install-the-plugins) says how to build it and where to put it: the DAW's VST2 folder pointed at `bin\Release\VST2`, or `D110Emu.dll` copied into a folder the DAW scans. It appears as **D110Emu** by D110Emu.
- **ROMs**: a `roms` folder is looked for next to the DLL and in each parent folder, so a DLL in `bin\Release\VST2` finds a `roms` folder next to `bin\` (see [ROMs](#roms) for the files it holds), and in `%APPDATA%\D110Emu`. Elsewhere, choose the folder in **File > Configuration...**, which opens by itself while there are no ROMs. New instances use the ROMs chosen last.
- **Each instance is a D-110 of its own**, and the project keeps it. The DAW saves the instance's settings and the whole memory (tones, timbres, patches, rhythm setup, system area, the parts' current timbres and tones, D-20 patterns) in the project and in its presets for the plugin. A new instance starts from the D-110's power-on memory; **File > Load SysEx file...** loads a saved memory, such as the standalone's `d110emu-memory.syx`. A project from another computer uses that computer's ROM folder.
- **What is different from D110Emu.exe**:
  - **File > Configuration...** has the ROMs, the output stage (analog mode, resampler) and MIDI cable speed. The audio device, sample rate and MIDI ports are the host's.
  - The keyboard starts hidden in a new plugin window; **View > Keyboard** shows it, and the project keeps that instance's choice.
  - **View > Window size** makes the window and its text 75–200% of the standalone window's size at the screen's scaling, as in D110Emu.exe; the plugins keep their own choice. If the host cannot resize its plugin window, the new size applies the next time the window opens.
  - The keyboard goes to the plugin window after a click in it: text fields, and the computer-keyboard piano (whose notes go straight to the synth, not through the host). Click the host's window to give its shortcuts back.
  - Settings that belong to the computer rather than to a project (the ROMs, the LCD colours, your own included, the window size) are kept in `d110emu-vst.ini` in `%APPDATA%\D110Emu`, never next to the plugin, where a plugin folder under Program Files allows no writing (see [Settings and memory](#settings-and-memory)). The reverb tuning (`d110emu-reverb.ini`) is kept there too, shared with the standalone.
  - The MIDI file player, ROM Play and the Patterns tab play on their own, not in time with the host's transport.
- **Outputs**: 40. Outputs 1-2 are the D-110's mix, with the reverb; then a stereo pair per part: **Part 1** (3-4) to **Part 8** (17-18), **Rhythm** (19-20), then **Part 9** (21-22) to **Part 15** (33-34, for 16-part mode); then **Multi 1** (35) to **Multi 6** (40), mono, or as three stereo pairs (see **Multi** below).
  - Each part's **Output** menu (the Play tab's Output column, or the Parts tab) has the D-110's choices, **Mix**, **Mix + reverb** and **Multi 1-6**, and in the plugins **Own output** and **Own output + reverb**: the part then plays out of its own pair, with its pan, so that it can have a mixer channel of its own. With **+ reverb** it still feeds the reverb, whose return stays in the mix.
  - **Multi 1-6** play out of the plugin's Multi outputs, as out of the unit's MULTI jacks: mono, dry, at the sound's full level (a centred sound's left and right together). Unlike on the unit, Multi 5 and 6 play while the reverb is on. Several parts and rhythm keys can share one. They are not in the mix, so have the DAW take the Multi outputs (older projects that put parts on Multi heard them in the mix), or set such parts to Mix.
  - **Multi** in **File > Configuration...** chooses how the plugins give the DAW MULTI 1-6: six mono outputs, or three stereo pairs, **Multi 1+2** (35-36), **Multi 3+4** (37-38) and **Multi 5+6** (39-40). In a pair, a part or rhythm key on either of its Multi outputs plays in stereo with its pan, dry, and the Output menus offer the three pairs instead of Multi 1-6. DAWs set up a plugin's outputs when they add it, so the choice applies to instances added afterwards, in projects opened afterwards too, on this computer: the tracks such projects had for Multi outputs may need setting up again. The Audio Unit always has the pairs, and the standalone has the same choice for its 7.1 speakers.
  - The rhythm part's menu (in the plugins only) has **Per key** and **Own output**. **Per key**: each key plays where the Rhythm tab's Output sends it: the mix, or a Multi output, so that the drums can go to up to six outputs of their own (say the bass drum on Multi 1, the snare on Multi 2, the hi-hats on Multi 3), or three stereo ones with the pairs, without an output per key. **Own output**: all its keys out of the **Rhythm** pair with their pans (keys on Mix + reverb still feed the reverb).
  - A part on its own output plays there whatever its output assign, Multi included. The project keeps which parts are on their own outputs; the output assigns are part of the memory, as on the unit.
- **Limits**: 64-bit hosts only; no automatable parameters, since everything is reachable over MIDI and SysEx from the host's track.

## VST3 plugin

`D110Emu.vst3` is the emulator as a VST3 instrument, with the same interface and outputs as the VST2 plugin.

- **Installing**: [BUILDING.md](BUILDING.md#3-install-the-plugins) says how to build it and where to put it: the bundle `D110Emu.vst3` is a folder, which goes whole into `C:\Program Files\Common Files\VST3` or another folder your DAW scans for VST3 plugins. It appears as **D110Emu** by D110Emu, an instrument.
- **ROMs**: a `roms` folder is looked for inside the bundle (`D110Emu.vst3\Contents\Resources\roms`, or `D110Emu.vst3\roms`) and above it, so the bundle in `bin\Release\VST3` finds the one next to `bin\`, and in `%APPDATA%\D110Emu`. Under Program Files, put a `roms` folder in `%APPDATA%\D110Emu` or inside `D110Emu.vst3`, or choose the folder in **File > Configuration...**, which opens by itself while there are no ROMs. The plugins keep that and the other computer-wide settings in `%APPDATA%\D110Emu`.
- **Outputs**: 23, all switched on from the start: **D-110 Mix**, then **Part 1** to **Part 8**, **Rhythm** and **Part 9** to **Part 15** (stereo), then **Multi 1** to **Multi 6** (mono), which the parts' **Output** menus and the Rhythm tab choose as in the [VST2 plugin](#vst2-plugin). With **Multi** set to three stereo pairs in **File > Configuration...** (as in the VST2 plugin), there are 20 outputs: **Multi 1+2**, **Multi 3+4** and **Multi 5+6** are stereo. A part or rhythm key plays out of its output whatever the DAW does with it; if the DAW has switched that output off, it is not heard, and its Output menu shows it in amber. To mix a part on a channel of its own, have the DAW make a track (or return) for that output of the instrument.
- **MIDI**: VST3 carries no MIDI as such, so what arrives depends on the DAW:
  - notes arrive as notes, each at its own sample;
  - controllers (volume, pan, modulation, sustain, RPN and NRPN...), channel pressure, pitch bend and program changes arrive through the plugin's MIDI mapping, as DAWs send controllers to VST3 instruments. They are listed as parameters ("MIDI Ch 1 CC 7" and so on), which are not meant for automation;
  - program changes also come as each MIDI channel's program: the plugin has a unit per channel, with the timbres I-A11 to I-B88 as its programs, which DAWs such as Cubase use;
  - SysEx arrives only from DAWs that pass it to VST3 plugins. Where yours does not, use the VST2 plugin for SysEx-heavy songs (MT-32 games, patch dumps), or load SysEx files with **File > Load SysEx file...**.
- **Everything else is as in the VST2 plugin**: the project keeps each instance's settings and memory, the keyboard starts hidden, **View > Window size**, and the MIDI file player, ROM Play and the Patterns tab play on their own.

### VST3 on Linux

The VST3 plugin also runs on Linux (64-bit Intel or ARM, a Raspberry Pi included), for DAWs such as REAPER, Bitwig Studio and Ardour. It is the same plugin: the outputs, the MIDI and the project state are as above. (The VST2 plugin is Windows only.)

- **Installing**: [BUILDING.md](BUILDING.md#linux) says how to build it (it needs the X11 and OpenGL development files) and install it with `install-plugins-linux.sh`, into `~/.vst3`, where DAWs look for VST3 plugins (or `/usr/local/lib/vst3` for every user of the computer); then rescan in the DAW. The bundle `D110Emu.vst3` is a folder with the library in `Contents/x86_64-linux/D110Emu.so` (`aarch64-linux` on ARM). The library brings its own C++ runtime and needs only the system's X11 and OpenGL libraries.
- **ROMs**: a `roms` folder inside the bundle or in a folder above it (`~/.vst3/roms`, say), one in `~/.local/share/D110Emu`, or the folder chosen in **File > Configuration...**. The computer-wide settings are kept in `d110emu-vst.ini` in `~/.local/share/D110Emu` (`$XDG_DATA_HOME/D110Emu`), not in the bundle.
- **The window** is drawn with OpenGL (2.1 or later: any graphics driver, or Mesa's software renderer) inside the DAW's window, through X11, as Linux DAWs host plugin windows (on a Wayland desktop they run under XWayland). Its size follows the desktop's scaling (the DAW's, or `Xft.dpi`), and **View > Window size** as on Windows.
- **Files**: the file dialogs are zenity's (kdialog's on KDE Plasma), in windows of their own: the DAW keeps running while one is open, and the plugin window waits for it (**Cancel** closes the dialog). Without either, the status bar says so, and `sudo apt install zenity` gives it one. A file dropped on the plugin window opens as in D110Emu's own window only where the file manager looks into plugin windows (KDE's Dolphin does, GNOME's Files does not) and the DAW's window does not take the drop itself; the File menu always works.

### VST3 on macOS

The VST3 plugin also runs on a Mac with macOS 11 or later, for DAWs that take VST3 plugins, such as REAPER, Ableton Live, Cubase, Bitwig Studio and Studio One. (Logic Pro and GarageBand take only Audio Units: see [Audio Unit on macOS](#audio-unit-on-macos).) It is the same plugin: the outputs, the MIDI and the project state are as above. The VST2 plugin is Windows only.

- **Installing**: [BUILDING.md](BUILDING.md#macos) says how to build it and install it with `install-plugins-macos.sh` (with the [Audio Unit](#audio-unit-on-macos)), into `~/Library/Audio/Plug-Ins/VST3`, where DAWs look for your VST3 plugins (`/Library/Audio/Plug-Ins/VST3` is for every user of the Mac); then rescan in the DAW. Quit the DAW before replacing a copy it has loaded. The bundle is signed ad hoc, like the app, and needs nothing installed: its window draws with the Mac's own Metal.
- **ROMs**: a `roms` folder in `~/Library/Application Support/D110Emu`, which the app uses too, one above the bundle (`~/Library/Audio/Plug-Ins/VST3/roms`), or the folder chosen in **File > Configuration...**. The plugin also looks inside the bundle, in `D110Emu.vst3/Contents/Resources/roms`: a `roms` folder put there changes the signed bundle, so sign it again afterwards (`codesign --force --sign - D110Emu.vst3`). The computer-wide settings are kept in `d110emu-vst.ini` in `~/Library/Application Support/D110Emu`, not in the bundle.
- **The window** is drawn with Metal inside the DAW's window, sharp on a Retina display. Its size is in points, as macOS counts, and **View > Window size** makes it 75–200% of the app's window. Keys work as in the app: Cmd+C, Cmd+V, Cmd+X, Cmd+A and Cmd+Z in text fields, and Ctrl+click is a right-click. After a click in the plugin window, the keyboard plays the computer-keyboard piano; shortcuts with Cmd still reach the DAW's menus.
- **Files**: the File menu opens macOS's own open and save panels, and the DAW waits while one is open. A file dropped on the plugin window opens as in the app.
- **Other Macs**: copy the bundle (in a zip, say). It is signed ad hoc, not by an Apple developer ID, so on a Mac that downloaded it, macOS keeps DAWs from loading it until the download mark is removed: `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/D110Emu.vst3`.

## Audio Unit on macOS

`D110Emu.component` is the emulator as an Audio Unit (AUv2) instrument, for Logic Pro, GarageBand and MainStage, which take only Audio Units, and the other Mac DAWs that take them (REAPER, Ableton Live...). It is the VST plugins' D110Emu, window included, and it keeps the same settings and ROMs. Hosts list it as **D110Emu** by **LA32Emu** (its codes: type `aumu`, subtype `D110`, manufacturer `LA32`).

- **Installing**: [BUILDING.md](BUILDING.md#macos) says how to build it and install it with `install-plugins-macos.sh`, into `~/Library/Audio/Plug-Ins/Components`, where Audio Unit hosts look for your plugins (`/Library/Audio/Plug-Ins/Components` is for every user of the Mac); quit the hosts first when replacing a copy they have loaded. The script has macOS notice a new or rebuilt component at once (otherwise it may take a restart), and its `--validate` runs Apple's own validation (`auval -v aumu D110 LA32`), the one Logic Pro and GarageBand run before they use a new plugin; it ends with **AU VALIDATION SUCCEEDED**.
- **ROMs and settings**: as for the VST3 plugin (see [VST3 on macOS](#vst3-on-macos)): `~/Library/Application Support/D110Emu/roms`, shared with the app, a `roms` folder above the component, or **File > Configuration...**; the unit also looks inside itself, in `D110Emu.component/Contents/Resources/roms` (sign it again after adding them: `codesign --force --sign - D110Emu.component`). The computer-wide settings are in `d110emu-vst.ini` in `~/Library/Application Support/D110Emu`, shared with the VST plugins.
- **Outputs**: 20 stereo outputs: **D-110 Mix**, **Part 1** to **Part 8**, **Rhythm**, **Part 9** to **Part 15**, then MULTI 1-6 as three stereo pairs, **Multi 1+2**, **Multi 3+4** and **Multi 5+6**, since Logic Pro cannot load a plugin that has mono outputs beside stereo ones. A part or rhythm key on either output of a pair plays there in stereo with its pan, dry. The parts' **Output** menus and the Rhythm tab choose them as in the [VST2 plugin](#vst2-plugin). In Logic Pro, choose the **Multi Output** version of the instrument and add aux channel strips for the outputs you use; GarageBand plays the mix alone.
- **MIDI**: notes, controllers, pitch bend and program changes arrive as MIDI, each at its own sample, and SysEx where the host sends it. Logic Pro sends none to Audio Units of this kind (AUv2), and GarageBand, which shares its engine, likely none either: there, load SysEx files with **File > Load SysEx file...**, or play MIDI files with the unit's own player.
- **GarageBand** runs in a sandbox, and asks before it loads a plugin that is not sandbox-safe, as this one is not (it reads its ROMs and settings from your folders): answer that it may lower its security settings. If D110Emu then finds no ROMs, put a `roms` folder inside the component, as above.
- **Everything else is as in the VST plugins**: the project keeps each instance's settings and memory, the keyboard starts hidden, **View > Window size** (hosts follow the window's new size), Cmd shortcuts in text fields, macOS's own file panels, files dropped on the window.
- **Other Macs**: as the VST3 plugin: copy the component, and remove the download mark there (`xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/D110Emu.component`).

## The terminal version

**D110EmuTUI** (`bin\Release\D110EmuTUI.exe`; `d110emu-tui` on Linux and macOS) is the emulator with a text interface, for a computer without a display or a good graphics driver, such as a Raspberry Pi as a sound module, or one reached over SSH. It is the same emulator as D110Emu.exe and keeps the same files in the same folder (`d110emu.ini`, `d110emu-memory.syx`, `d110emu-reverb.ini`; see [Settings and memory](#settings-and-memory)), so the two can take turns with the same settings and memory.

The screen shows the unit: its display; the reverb, tune and patch; the MIDI file player and the MIDI inputs (with a light while they send); each part's channel, timbre, level, pan, output, volume, expression and partials, a level meter and the notes it plays; the output level, the partials and the log. What keeps it from sounding or hearing MIDI shows in red under the log. **O** opens the options: the audio output, MIDI inputs and ROMs of the configuration window, and the sound, MIDI and system settings of the System tab, each with a line of help. The keys (**?** shows them):

| Key | |
|---|---|
| ↑ ↓, ← → | Choose a part, which the display shows; ← → are the display's buttons (in performance mode: the patch, or its upper and lower tones) |
| Enter | A test note on the part: a C major chord, or bass drum and hi-hat on the rhythm part |
| M, S, U | Mute or solo the part; U hears them all again |
| P | The patch on the display |
| Esc | The display back from a SysEx message |
| T | MT-32 translation on or off |
| X | Reset MIDI: sound off, controllers reset, levels 100, pans centred |
| + - | Output gain |
| R | ROM Play |
| F | Play a MIDI file or load a SysEx file |
| Space, Backspace, L | Play or pause the MIDI file, stop it, loop it |
| O | Options |
| Tab | The log |
| Q | Quit (the memory is saved, as the standalone saves it) |

On the command line (`--help` lists everything): `--list` lists the audio devices and MIDI inputs; `--audio-device NAME`, `--midi-in NAME` (repeatable; part of a name does) and `--roms DIR` choose them as the options do, and are kept; `--config DIR` keeps the settings and memory in another folder; `--ascii` and `--no-color` suit terminals without line characters or colours (it finds most out itself: the Linux console gets its own characters, `NO_COLOR` is followed). MIDI and SysEx files named on the command line play and load at the start.

On Windows it runs in Windows Terminal or the console of Windows 10 and later.

### On Linux

[BUILDING.md](BUILDING.md#linux) says how to build it (it needs only `build-essential` and premake5), and [Linux](#linux) how sound, MIDI and permissions work there. In the terminal version, **O** > **Device** and **O** > **MIDI inputs** choose them; while the audio output is not running (no device at the start, say), it is tried again whenever the list of devices changes.

### On macOS

[BUILDING.md](BUILDING.md#macos) says how to build it (Apple's command line tools and premake5 are all it needs), and [macOS](#macos) how sound and MIDI work there. It runs the same way as on Linux, `--headless` included.

### Without a screen

`--headless` runs it without the interface until it is stopped (Ctrl+C, or a service manager's SIGTERM), with the log (SysEx display messages included) on the standard output; the memory is saved as it stops. Choose the audio device and MIDI inputs in the interface first (they are kept), or give them on the command line. As a systemd service that starts at boot, for example in `/etc/systemd/system/d110emu.service`:

```ini
[Unit]
Description=D110Emu
After=sound.target

[Service]
User=pi
ExecStart=/home/pi/d110emu/bin/linux-Release/d110emu-tui --headless
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

`sudo systemctl enable --now d110emu` starts it now and at every boot, and `journalctl -u d110emu` shows its log.

## MT32Translator

MT32Translator sits between a program that plays an MT-32 and a real D-110, D-10 or D-20, like the translation boxes of the time. What d110emu's **MT-32 translation** does inside the emulator, it does on the way to the real unit: waves and their pitch, pans, reverb, presets, the MT-32's power-on state. It also handles what real units need:

- **Handshake loading.** Many games (on the X68000 and PC-98 among others) load their sounds with a handshake transfer: they wait for the MT-32 to acknowledge each packet. MT32Translator acknowledges them itself, on the **Replies** port, and sends the data on to the unit.
- **Channels.** The MT-32's parts listen on channels 2–9 (rhythm 10) unless the program moves them. A D-110 is set to the same channels over SysEx. A D-10 or D-20 keeps its part channels on its panel, so the translator moves every message to the part's channel there.
- **Memory.** In multi-timbral mode a D-20 takes timbre and tone data for its parts, but takes changes to its memory only while its **Memory Protect** is off, and that turns back on whenever the unit is switched off. So for a D-10/D-20 the translator keeps the MT-32 program's patches and timbres itself. A program change is sent as the part's timbre and tone (about 90 ms of SysEx), and the unit's own memory is left alone, unless the **Tone cache** stores the tones there, which makes that instant after the first time.
- **Master volume.** The D-series has none, so it scales the parts' MIDI volume (CC 7).
- **Pacing.** A SysEx message is followed by a pause (its transmission time plus **SysEx pause**), so the unit can store it before more arrives. Notes wait behind it, in order.

### Setting up

1. Connect the program's MIDI output to a MIDI input of the PC: a USB MIDI interface from another computer (an X68000, a PC-98), or a virtual port ([loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html)) from an emulator on the same PC.
2. Tick that input under **From the MT-32 program**, and choose the unit's MIDI output under **To the unit**.
3. For handshake loading, choose the output back to the program's MIDI input under **Replies**.
4. Pick the unit (**D-110** or **D-10 / D-20**) and its **Unit number** (17–32).
5. On a D-10/D-20:
   - use multi-timbral mode;
   - in the MIDI function menu, turn **Exclusive** on and set the unit number;
   - set the **Part channels on the unit** to match its panel. **1-8, R 10** and **2-9, R 10** set the usual layouts.
6. Start the program. If it doesn't reset the MT-32 itself, click **MT-32 reset** first (or tick **MT-32 power-on setup at start**).

### Options

- **Write the MT-32's memories to the unit** (on for the D-110, off for the D-10/D-20):
  - On: the program's patch memory becomes the unit's timbre memory (A11–B88) and its timbres become the tones i11–i88, overwriting what the unit had there.
  - Off: the translator keeps them.
  - If your D-20 turns out to accept memory data in multi-timbral mode, it can be switched on, which makes program changes quicker.
- **MT-32 presets**:
  - **The unit's closest presets**: instant, as described under MT-32 translation above.
  - **Closest, MT-32's own if none fits** (the default): presets with no real counterpart play the MT-32's own timbre, translated from its control ROM and sent as the part's tone when a part selects it. These are Elec Org 3 and 4, Elec Gtr 2 (a clear guitar on the MT-32, distorted with feedback on the D-series), Doctor Solo, Shakuhachi and the bells.
  - **The MT-32's own presets**: all of them. Each selection is a 246-byte message (about 80 ms), unless the tone cache has the tone.
  - The MT-32's own timbres need an MT-32 or CM-32L control ROM: put it in a `roms` folder next to the .exe or in `%APPDATA%\D110Emu`, or pick it with **Control ROM...**. Rhythm sounds always use the unit's own.
  - The **Presets** tab lists all 128 MT-32 presets. For each you can choose the unit's preset it plays, an octave shift, and (in the middle mode) whether it plays the MT-32's own timbre instead. **Built in** restores a row, **All built in** everything.
- **Toms**: the MT-32's toms play the unit's TomTom2 set (the same sample, at the same pitches); **Roomy** plays the TomTom1 set.
- **Master volume as CC 7**: see above.
- **Tone cache** (while the translator keeps the memories): the tones the translator sends are also stored in the unit's internal tones with write requests, the first time each is used. Selecting one again is then a short message and instant, and rhythm keys that play the program's timbres get them too.
  - Choose which tones it may use (**Tones i51 to i88** by default). Those tones are overwritten, so save them first if you need them.
  - Turn the unit's Memory Protect off. A D-10/D-20 turns it back on when switched off.
  - **Preload** stores the program's timbres at once, at a quiet moment such as the program's title screen after it has loaded its sounds.
  - The cache is kept between sessions (`mt32translator-cache.syx`). After using the unit's memory otherwise, click **Forget**.
  - **From the unit** (optional): connect the unit's MIDI OUT to see its answers. If it refuses a write (Memory Protect on), the translator turns the cache off and sends the tones directly.
- **Reduce MIDI load**: leaves out controller values and pitch bends the unit already has, and when several wait behind a SysEx message, sends only the newest. Older D-series firmware processes envelopes late when busy, so attacks can break up, and less MIDI helps.
- **SysEx pause**: raise it if the unit misses parts of long transfers. Shorter messages get a proportionally shorter pause.

Ports are remembered by name. One that is not there when the translator starts (unplugged, or its program not started yet) is opened as soon as it appears, and an output that goes away (a MIDI interface unplugged) is connected again when it comes back; the log says so each time.

### On Linux and macOS

`mt32translator` (Linux) and `MT32Translator.app` (macOS) are the same window ([BUILDING.md](BUILDING.md) says how to build them). The ports are the ALSA sequencer's or CoreMIDI's, by the names `aconnect -l` or Audio MIDI Setup show. A program on the same computer (an emulator, say) needs no virtual cable: it can send to the translator itself, which is the sequencer port **MT32Translator:MIDI In** on Linux (the ports list says its number; `aconnect` the program's port to it, or set it as the program's MIDI output) and the MIDI destination **MT32Translator** on a Mac. The **Replies** output then goes to the program's MIDI input port, where it has one.

### The terminal version

**MT32TranslatorTUI** (`bin\Release\MT32TranslatorTUI.exe`; `mt32translator-tui` on Linux and macOS) is the translator with a text interface, for a computer between the game and the unit that has no screen, such as a Raspberry Pi with a USB MIDI interface, or one reached over SSH. It keeps its settings in the window's file (`mt32translator.ini`), so either can set it up.

The screen shows the ports (with a light while they carry MIDI; ports chosen but not there in yellow), the unit, the presets, the tone cache, the timing, the counters and the log. **O** opens the options: the ports, the unit and its part channels, the presets (the MT-32's own ones chosen one by one too, as on the Presets tab), the tone cache and the timing, each with a line of help. The keys (**?** shows them):

| Key | |
|---|---|
| O | Options |
| P | The MIDI inputs the MT-32 program plays into |
| R | MT-32 reset: the MT-32's power-on setup, to the unit |
| N | All notes off on the unit's channels |
| S | Send a SysEx file (.syx, or a game's .dat) through the translation |
| T | Translate a file: it asks where the translated copy goes |
| Tab | The log |
| Q | Quit (the settings and the tone cache are kept) |

On the command line (`--help` lists everything): `--list` lists the MIDI inputs and outputs; `--in NAME` (repeatable), `--out NAME`, `--replies NAME` and `--unit-in NAME` choose the ports (part of a name does, and a port not there yet is opened when it appears), `--unit d110` or `--unit d20` and `--unit-number N` the unit; all of these are kept, as the options keep them. `--config DIR` keeps the settings elsewhere. SysEx files named on the command line go to the unit at the start.

`--headless` runs it without the interface until it is stopped (Ctrl+C, or a service manager's SIGTERM), with the log on the standard output. As a systemd service that starts at boot, for example in `/etc/systemd/system/mt32translator.service`:

```ini
[Unit]
Description=MT32Translator
After=sound.target

[Service]
User=pi
ExecStart=/home/pi/d110emu/bin/linux-Release/mt32translator-tui --headless
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Set the ports up in the interface first, or on the command line (`mt32translator-tui --headless --in UM-ONE --out UM-ONE --unit d20`: a game machine on the interface's MIDI IN, the unit on its MIDI OUT). `sudo systemctl enable --now mt32translator` starts it now and at every boot, and `journalctl -u mt32translator` shows its log.

### Files

- **Send SysEx file...** (or drop a `.syx`/`.dat` on the window) sends a file through the translation, as if the program had sent it. This works for a game's own data file too, such as `VOICE_MT.DAT` with its handshake packets.
- **Translate file...** saves a translated copy:
  - A **SysEx file** becomes DT1 messages that load into the unit's memory, with its Memory Protect off. A D-20's rhythm setup goes to its rhythm setup memory.
  - A **MIDI file** becomes one that plays on the unit, with the MT-32's power-on setup first; the song starts after it.

The monitor lists what came in (SysEx, program changes, handshakes), with counters for what was sent. Data requests from the program (RQ1, RQD) are not passed on, as the unit's answer would not be the MT-32's. The CM-32L's sound effects have no D-series counterpart.

`mt32translate` does the file translation from the command line:

```sh
mt32translate VOICE_MT.DAT voice-d20.syx                  # D-10/D-20, unit 17
mt32translate --target d110 --unit 18 song.mid song-d110.mid
mt32translate --rom roms/MT32_CONTROL.ROM song.mid song-d20.mid    # the MT-32's own where none fits
```

Its other options are:
- `--exact ROM`, for all presets;
- `--stand-ins`, for the unit's presets only;
- `--roomy-toms`;
- `--channels 1,2,3,4,5,6,7,8,10`, the D-20's part channels;
- `--keep-memory` or `--memory-in-unit`;
- `--gap MS`.

Settings are saved to `mt32translator.ini` in D110Emu's folder (`%APPDATA%\D110Emu` on Windows; see [Settings and memory](#settings-and-memory)).

## ToneEditor

ToneEditor edits the tones of a real D-110, D-10, D-20 or MT-32 in real time, like Roland's PG-10 programmer on screen. It is D110Emu's Tone tab on its own, working over MIDI: every change goes to the edited part's tone temporary area at once, as a DT1 message, so the unit plays it from the next note. Its other tabs change the unit's parts, timbres, rhythm setup and system settings the same way, and a D-10/D-20's performance patch.

### Setting up

1. Connect a MIDI output of the PC to the unit's MIDI IN, and choose it under **To the unit**.
2. Optionally, connect the unit's MIDI OUT to a MIDI input and choose it under **From the unit**. The editor can then read tones from the unit (with data requests, RQ1) and follow what you edit on its panel.
3. Choose the unit (**D-110**, **D-10/D-20** or **MT-32**) and its **Unit number** (17–32; an MT-32 is 17).
4. Choose the **Part** to edit and its MIDI **Channel** on the unit. On a D-110 or MT-32, **Read** gets the channels from the unit.
5. On a D-10/D-20:
   - in the MIDI function menu, turn **Exclusive** on and set the unit number;
   - multi-timbral and performance mode both work. In performance mode, tick **Performance mode** under **Unit**: **Upper** and **Lower** then edit the patch's two tones (parts 1 and 2's tones), and the editor's notes go to the performance channel;
   - to change its memory in multi-timbral mode (the Timbres tab, **Write**), turn **Memory Protect** off (TUNE/FUNCTION, then the DISPLAY up button, then the Value knob). It turns back on when the unit is switched off. Performance mode editing works with it on.
6. Optionally, tick your MIDI keyboard under **Keyboards**. It then plays the edited part through the editor, so **Restrike** can play held notes again. A D-20's own keyboard plays the unit directly.

### Using it

- **Get from the unit** reads the part's tone. This also happens when you choose a part or connect the unit's MIDI OUT.
- Edit as in D110Emu's Tone tab.
  - Changes that come faster than the unit takes them (while you drag a slider) are merged, so the unit gets the latest value without being flooded.
  - **SysEx pause** sets how far apart they go. Raise it if the unit shows "Exclusive Buffer Full" or lags.
- **Write** stores the tone in i11–i88 with the unit's tone write (on an MT-32, as a memory timbre), after asking.
  - Turn the unit's Memory Protect off.
  - With the unit's MIDI OUT connected, the log shows its answer.
- **Send to the unit** sends the whole tone, for example after switching the unit on.
- **Library**:
  - **Read all from the unit** fetches tones i11–i88 (about 7 seconds). They are kept for the next session (`toneeditor-library.syx`), and **Save...** writes them as a SysEx file.
  - **Open SysEx...** (or dropping a file on the window) adds a file's tones.
  - Click a tone to load it into the part.
  - MT-32 timbres (such as a game's `VOICE_MT.DAT`) can have their waves translated for the D-series, as MT32Translator does.
- A D-10/D-20 shows every parameter, as a D-110 does: the three units share one sound engine. The D-20's own panel leaves out the TVF and TVA envelopes' Time 4 and Level 3 and the pitch envelope's sustain level, but the unit plays them (their tooltips say so).

### The unit's setup

The **Parts**, **Timbres**, **Performance** (D-10/D-20), **Rhythm** and **System** tabs change the rest of the unit's setup the same way: every change goes to the unit at once.

- **Parts**: for each part:
  - its MIDI channel;
  - the tone it plays (a, b, i and r; on an MT-32, A, B, M and R), and **PC** for a program change on its channel;
  - key shift, fine tune, bender range and assign mode;
  - output: Mix, Mix + reverb or Multi 1–6 on a D-110, reverb on or off on a D-10/D-20 or MT-32;
  - level, pan, the key range (D-110) and the partial reserve.

  Click a part's number to edit its tone.
- **Timbres** (on an MT-32, **Patches**): timbre memory A11–B88, like D110Emu's Timbres tab, with each timbre's tone, key shift, fine tune, bender range, assign mode and output.
  - Click a timbre to play it on the edited part (a program change).
  - Changes go to the unit's timbre memory at once, and to the parts that play the timbre, so you hear them.
  - **Write** stores what the part plays as a timbre, with the unit's own timbre write (a D-10/D-20 does this in multi-timbral mode).
  - A D-10/D-20 takes all of this in multi-timbral mode, as long as its **Memory Protect** is off; the tab says how.
  - Right-click a timbre to copy and paste it.
- **Performance** (D-10/D-20 in performance mode): the performance patch, as the unit's patch edit shows it.
  - Its name and level; the key mode (**Whole**: the upper tone only, **Dual**: both, **Split**: the lower tone below the split point, C2–C#7); the tone balance, shown as the unit shows it (the lower and upper volumes add up to 100); the reverb's type, time and level.
  - For the upper and lower tone: the tone (a, b, i or r), key shift, fine tune, bender range, assign mode and reverb switch. **Edit** opens the tone on the Tone tab in performance mode (the upper tone is part 1's, the lower part 2's).
  - **Performance channel**: the unit's receive channel in performance mode (Rx CH in its MIDI function menu). Click a patch in the list to play it, as a program change there does.
  - **Write** stores the patch in A11–B88 with the unit's patch write. Unlike timbre editing, this works with the unit's Memory Protect on.
- **Rhythm**: the rhythm part's level, then the tone, level, pan and output of every key from C1 (24) to C8 (108). **Play** plays a key.
- **System**:
  - master tune, and the reverb's type, time and level;
  - the partial reserves, sent as a package of all nine parts (at most 32 partials in all);
  - the MIDI channels (**1-8, R 10** and **2-9, R 10** set the usual layouts), and an MT-32's master volume.
- **Read from the unit** reads the setup. It is also read at start and when you connect the unit's MIDI OUT. Values not read yet show "?".
- A D-20 takes part settings in multi-timbral mode, and sets its part channels on its panel: there, the channels only tell the editor where to play (the Rhythm tab's **Play** uses the rhythm part's). It may take system settings only in data transfer mode (not confirmed).
- A D-10/D-20 set up here as a D-110 reports the same dummy value for all part channels. ToneEditor doesn't take those and says so: choose **D-10/D-20** under **Unit**.

On Linux and macOS, `toneeditor` and `ToneEditor.app` are the same window ([BUILDING.md](BUILDING.md) says how to build them), with the ALSA sequencer's or CoreMIDI's ports. A program on the same computer (a software keyboard, say) can play the edited part through the editor's own input, as a MIDI keyboard ticked under **Keyboards** does: the sequencer port **ToneEditor:MIDI In** on Linux, the MIDI destination **ToneEditor** on a Mac.

Settings are saved to `toneeditor.ini` in D110Emu's folder (`%APPDATA%\D110Emu` on Windows; see [Settings and memory](#settings-and-memory)).

## PatternCapture

A D-20's bulk dump holds its programmable rhythm patterns P-51–P-88 but not its 32 presets P-11–P-48, which are in its ROM. PatternCapture records them from what the D-20 plays on its MIDI OUT: in Pattern Play it sends the pattern's notes on the rhythm part's channel, and MIDI clock at 24 clocks per quarter note, which is exactly the patterns' step grid. So each note lands on its step, whatever the tempo.

### Setting up

1. Connect the D-20's MIDI OUT to a MIDI input of the PC, and choose it under **From the D-20**. Close it in D110Emu first if D110Emu uses it (a Windows MIDI input usually serves one program at a time).
2. **Rhythm channel**: the D-20's rhythm part channel, 10 unless you changed it.
3. On the D-20, set the **Clock Mode** to INTERNAL (hold TEMPO and press DISPLAY; its manual p.165): only then does it send the clock and the pattern's notes. Push RHYTHM, and choose **Pattern Play** with DISPLAY.

### Using it

1. Choose the pattern on the D-20 with BANK and NUMBER, and the same slot in PatternCapture (**P-11 8Beat 1** and so on, named as in the D-20's manual).
2. **Hold STOP and press START.** The D-20 then sends Start and plays the pattern from its first beat. START alone sends Continue and resumes where it stopped, so PatternCapture cannot tell where the bar begins; it says so and doesn't store that take.
3. Let it play for at least two bars (four for a pattern longer than 4/4), then press **STOP**.
4. The take appears on the right, one row per drum key. PatternCapture compares its bars to find the pattern's length: 4/4 if the notes repeat at 4/4 (a simple beat also repeats every half bar), else the shortest length they repeat at. Lengths the notes repeat at are shown in green.
5. With **Store at STOP**, a take whose bars agree goes into the slot at once, and **Then select the next empty slot** moves on, so you only work the D-20: choose the next pattern, hold STOP and press START, and STOP. Otherwise, or when a take is not stored, choose the length and click **Store in P-xx**, or record it again.
6. The D-20's manual doesn't give the presets' time signatures. Check the ones that sound in three (probably the Jazz Waltz) or look odd, and change their length if needed.
7. **Save dump...** writes the patterns into a D-20 dump, in P-51–P-88 as a D-20 sends its own. Saved as `D-20 preset patterns.syx` in D110Emu's `roms` folder, D110Emu loads it at every start (the included one holds all 32); the Patterns tab's **Load presets P-11–P-48...** loads one until the next start. Sent to a D-20 (Memory Protect off), it would replace P-51–P-88 with copies of the presets.

The captures are kept in `patterncapture-session.syx` in `%APPDATA%\D110Emu`, so the work can be spread over several sessions; **Open dump...** loads patterns back from a saved dump, and right-clicking a slot clears it.

## Credits

- [davidhsilaban's D-110 MUNT fork](https://github.com/davidhsilaban/munt)
- [MUNT project](https://github.com/munt/munt)
- [Dear ImGui](https://github.com/ocornut/imgui)
- and everyone else mentioned in their licenses

## Licences

mt32emu: LGPL 2.1 or later (`mt32emu\COPYING.LESSER.txt`). Dear ImGui: MIT. SDL 3 (Linux: the system's; macOS: SDL's official framework, which the build downloads and puts inside the app): zlib. miniaudio: public domain or MIT-0. premake: BSD 3-clause. VST is a trademark of Steinberg Media Technologies GmbH; the VST2 plugin's interface (`src\Vst2.h`) is written from the interface's public descriptions, without Steinberg's SDK. The VST3 plugin uses Steinberg's VST3 interface headers (`vendor\vst3sdk`): MIT. The Audio Unit uses Apple's AudioUnitSDK (`vendor\AudioUnitSDK`): Apache 2.0.

Roland, Roland Corp., MT-32, D-110, D-10, D-20 and are registered trademarks of Roland Corporation. All rights reserved.
