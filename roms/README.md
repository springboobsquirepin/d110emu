# ROMs

D110Emu runs the D-110's own firmware and samples, so it needs images of the D-110's ROM chips: a **Control ROM** and a **PCM ROM**, each joined from two chip dumps. Files are recognised by their content (SHA1), not by their names.

## The chips you need

| Chip | Holds | Size | Dump file (as MAME names it) | SHA1 of the dump |
|---|---|---|---|---|
| IC19 | Firmware v1.10 | 32 KiB (32,768 bytes) | `d-110.v1.10.ic19.bin` | `28635510f30d6c1fb88e00da03e5b4e045c380cb` |
| IC12 | R15179873 (LH5310-97): preset and rhythm tones, the sample table, the ROM Play songs | 128 KiB (131,072 bytes) | `r15179873-lh5310-97.ic12.bin` | `05587a0542b01625dcde37de5bb339880e47eb93` |
| IC8 | R15179880: samples, first half | 512 KiB (524,288 bytes) | `r15179880.ic8.bin` | `9c59f50518a070461b2ec6cb4e43ee7cc1e905b6` |
| IC7 | R15179878: samples, second half | 512 KiB (524,288 bytes) | `r15179878.ic7.bin` | `6760d14900161b8715c2bfd4ebe997877087c90c` |

Not needed: IC6 (`r15179879.ic6.bin`, 32 KiB), which D110Emu does not use, and firmware v1.06 (`d-110.v1.06.ic19.bin`), which it does not recognise.

## Joining them

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

## Checking them

| Image | Size | SHA1 |
|---|---|---|
| `CONTROLromcombo.bin` (D-110 Control v1.10) | 163,840 bytes | `8d549f3382a23b8faa64e3988f913d403a92887f` |
| `PCMromcombo.bin` (D-110 PCM ROM) | 1,048,576 bytes | `8eb2e3857a36272eb66d64d8dcb82a6b14c8d26e` |

`certutil -hashfile CONTROLromcombo.bin SHA1` in a Command Prompt shows a file's SHA1 (`Get-FileHash -Algorithm SHA1 CONTROLromcombo.bin` in PowerShell, `sha1sum` on Linux, `shasum` on macOS). Chips joined in the other order, or another firmware version, give another SHA1, and D110Emu then does not list the file. It also accepts one other v1.10 Control image, from a different dump: SHA1 `8b064509db7520e0633b8bea9e3ce3945fc7de54`.

## Where they go

At startup the emulator looks for a folder named `roms`: inside the Mac app (`D110Emu.app/Contents/Resources`), next to the .exe (or a plugin) and in each parent folder, in the working folder, and in D110Emu's own folder (see [Settings and memory](#settings-and-memory)). So a `roms` folder next to `bin\` is found when the program runs from `bin\`, and one in `%APPDATA%\D110Emu` (`~/Library/Application Support/D110Emu` on a Mac, `~/.local/share/D110Emu` on Linux) wherever the program is. **File > Configuration...** chooses another folder, and the Control and PCM ROM when the folder holds more than one.
