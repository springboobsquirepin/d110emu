/* Copyright (C) 2003, 2004, 2005, 2006, 2008, 2009 Dean Beeler, Jerome Fisher
 * Copyright (C) 2011-2020 Dean Beeler, Jerome Fisher, Sergey V. Mikayev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MT32EMU_MEMORY_REGION_H
#define MT32EMU_MEMORY_REGION_H

#include <cstddef>

#include "globals.h"
#include "Types.h"
#include "Structures.h"

namespace MT32Emu {

enum MemoryRegionType {
	MR_PatchTemp, MR_RhythmTemp, MR_TimbreTemp, MR_Patches, MR_Timbres, MR_System, MR_Display, MR_Reset,
	// D-110 and D-20 address map additions
	MR_D110Patches, MR_D20Patches, MR_D20RhythmSetup, MR_D20SystemExt, MR_WriteRequest,
	// 16-part mode: parts 9-15
	MR_ExtPatchTemp, MR_ExtTimbreTemp, MR_ExtSystem,
	// D-20 performance mode: the patch temporary area
	MR_D20PatchTemp,
	// D-110 memory card
	MR_CardTones, MR_CardTimbres, MR_CardPatches,
	// Fine pan (with nice panning)
	MR_PartFinePan, MR_RhythmFinePan,
	// D-20 rhythm patterns (P-51-P-88, and the presets P-11-P-48 as an extension) and rhythm track
	MR_D20Patterns, MR_D20PresetPatterns, MR_D20RhythmTrack
};

class Synth;

class MemoryRegion {
private:
	Synth *synth;
	Bit8u *realMemory;
	Bit8u *maxTable;
public:
	MemoryRegionType type;
	// entries: how many entries the backing memory holds (bounds for read/write).
	// addressableEntries: how many of them SysEx addresses reach (the timbre region stores all 256 timbres,
	// but only the 64 memory timbres are addressable at 08 00 00).
	Bit32u startAddr, entrySize, entries, addressableEntries;

	MemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable, MemoryRegionType useType, Bit32u useStartAddr, Bit32u useEntrySize, Bit32u useEntries, Bit32u useAddressableEntries = 0) {
		synth = useSynth;
		realMemory = useRealMemory;
		maxTable = useMaxTable;
		type = useType;
		startAddr = useStartAddr;
		entrySize = useEntrySize;
		entries = useEntries;
		addressableEntries = useAddressableEntries != 0 ? useAddressableEntries : useEntries;
	}
	int lastTouched(Bit32u addr, Bit32u len) const {
		return (offset(addr) + len - 1) / entrySize;
	}
	int firstTouchedOffset(Bit32u addr) const {
		return offset(addr) % entrySize;
	}
	int firstTouched(Bit32u addr) const {
		return offset(addr) / entrySize;
	}
	Bit32u regionEnd() const {
		return startAddr + entrySize * addressableEntries;
	}
	bool contains(Bit32u addr) const {
		return addr >= startAddr && addr < regionEnd();
	}
	int offset(Bit32u addr) const {
		return addr - startAddr;
	}
	Bit32u getClampedLen(Bit32u addr, Bit32u len) const {
		if (addr + len > regionEnd())
			return regionEnd() - addr;
		return len;
	}
	Bit32u next(Bit32u addr, Bit32u len) const {
		if (addr + len > regionEnd()) {
			return regionEnd() - addr;
		}
		return 0;
	}
	Bit8u getMaxValue(int off) const {
		if (maxTable == NULL)
			return 0xFF;
		return maxTable[off % entrySize];
	}
	Bit8u *getRealMemory() const {
		return realMemory;
	}
	bool isReadable() const {
		return getRealMemory() != NULL;
	}
	void read(unsigned int entry, unsigned int off, Bit8u *dst, unsigned int len) const;
	void write(unsigned int entry, unsigned int off, const Bit8u *src, unsigned int len, bool init = false) const;
}; // class MemoryRegion

class PatchTempMemoryRegion : public MemoryRegion {
public:
	PatchTempMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_PatchTemp, MT32EMU_MEMADDR(0x030000), sizeof(MemParams::PatchTemp), 9) {}
};
class RhythmTempMemoryRegion : public MemoryRegion {
public:
	RhythmTempMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_RhythmTemp, MT32EMU_MEMADDR(0x030110), sizeof(MemParams::RhythmTemp), 85) {}
};
class TimbreTempMemoryRegion : public MemoryRegion {
public:
	TimbreTempMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_TimbreTemp, MT32EMU_MEMADDR(0x040000), sizeof(TimbreParam), 8) {}
};
class PatchesMemoryRegion : public MemoryRegion {
public:
	PatchesMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_Patches, MT32EMU_MEMADDR(0x050000), sizeof(PatchParam), 128) {}
};
class TimbresMemoryRegion : public MemoryRegion {
public:
	// Stores groups A, B, Memory and Rhythm (and the D-110's card and extra banks); SysEx reaches only the 64 memory
	// timbres (08 00 00 - 08 7F 7F).
	TimbresMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_Timbres, MT32EMU_MEMADDR(0x080000), sizeof(MemParams::PaddedTimbre), TIMBRE_COUNT, 64) {}
};
class SystemMemoryRegion : public MemoryRegion {
public:
	SystemMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable, Bit32u size) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_System, MT32EMU_MEMADDR(0x100000), size, 1) {}
};
class D110PatchesMemoryRegion : public MemoryRegion {
public:
	D110PatchesMemoryRegion(Synth *useSynth, Bit8u *useRealMemory) : MemoryRegion(useSynth, useRealMemory, NULL, MR_D110Patches, MT32EMU_MEMADDR(0x060000), sizeof(D110PatchParam), 64) {}
};
class D20PatchesMemoryRegion : public MemoryRegion {
public:
	D20PatchesMemoryRegion(Synth *useSynth, Bit8u *useRealMemory) : MemoryRegion(useSynth, useRealMemory, NULL, MR_D20Patches, MT32EMU_MEMADDR(0x070000), D20_PATCH_SIZE, 128) {}
};
// D-110 memory card (an extension of the address map, 10H above the internal memories): tones c11-c88 at 18 00 00,
// timbres C-A11-C-B88 at 15 00 00 and patches C-11-C-88 at 16 00 00.
class CardTonesMemoryRegion : public MemoryRegion {
public:
	CardTonesMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_CardTones, MT32EMU_MEMADDR(0x180000), sizeof(MemParams::PaddedTimbre), 64) {}
};
class CardTimbresMemoryRegion : public MemoryRegion {
public:
	CardTimbresMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_CardTimbres, MT32EMU_MEMADDR(0x150000), sizeof(PatchParam), 128) {}
};
class CardPatchesMemoryRegion : public MemoryRegion {
public:
	CardPatchesMemoryRegion(Synth *useSynth, Bit8u *useRealMemory) : MemoryRegion(useSynth, useRealMemory, NULL, MR_CardPatches, MT32EMU_MEMADDR(0x160000), sizeof(D110PatchParam), 64) {}
};
class D20PatchTempMemoryRegion : public MemoryRegion {
public:
	// The D-20's current performance patch at 03 04 00 (after the rhythm setup, which ends at 03 03 63).
	D20PatchTempMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_D20PatchTemp, MT32EMU_MEMADDR(0x030400), D20_PATCH_SIZE, 1) {}
};
class D20RhythmSetupMemoryRegion : public MemoryRegion {
public:
	// The D-20's rhythm setup memory; applied to the D-110's single rhythm setup (the temporary area).
	D20RhythmSetupMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_D20RhythmSetup, MT32EMU_MEMADDR(0x090000), sizeof(MemParams::RhythmTemp), 85) {}
};
class D20SystemExtMemoryRegion : public MemoryRegion {
public:
	// D-20 part output levels and pans at 10 00 21 - 10 00 31, applied to the parts' temporary areas.
	D20SystemExtMemoryRegion(Synth *useSynth) : MemoryRegion(useSynth, NULL, NULL, MR_D20SystemExt, MT32EMU_MEMADDR(0x100000) + SYSTEM_SIZE_D110, SYSTEM_D20_EXT_SIZE, 1) {}
};
class WriteRequestMemoryRegion : public MemoryRegion {
public:
	// 40 00 00 tone write, 40 01 00 timbre write, 40 02 00 patch write, 40 03 00 the D-20's performance patch write (two
	// bytes each: number, internal/card).
	WriteRequestMemoryRegion(Synth *useSynth) : MemoryRegion(useSynth, NULL, NULL, MR_WriteRequest, MT32EMU_MEMADDR(0x400000), MT32EMU_MEMADDR(0x401001) - MT32EMU_MEMADDR(0x400000), 1) {}
};
// 16-part mode: the temporary areas of parts 9-15 at 13 00 00 and 14 00 00 (like 03 00 00 and 04 00 00 for
// parts 1-8), and their partial reserves and MIDI channels at 11 00 00-07 and 11 00 08-0F.
class ExtPatchTempMemoryRegion : public MemoryRegion {
public:
	ExtPatchTempMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_ExtPatchTemp, MT32EMU_MEMADDR(0x130000), sizeof(MemParams::PatchTemp), EXTRA_PART_COUNT) {}
};
class ExtTimbreTempMemoryRegion : public MemoryRegion {
public:
	ExtTimbreTempMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_ExtTimbreTemp, MT32EMU_MEMADDR(0x140000), sizeof(TimbreParam), EXTRA_PART_COUNT) {}
};
class ExtSystemMemoryRegion : public MemoryRegion {
public:
	ExtSystemMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_ExtSystem, MT32EMU_MEMADDR(0x110000), sizeof(MemParams::SystemExt), 1) {}
};
// D-20 rhythm data: patterns P-51-P-88 at 0A 00 00 (as on the D-20), the rhythm track at 0C 00 00 and, as an
// extension, the preset patterns P-11-P-48 at 0D 00 00. Stored as received.
class D20PatternsMemoryRegion : public MemoryRegion {
public:
	D20PatternsMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, MemoryRegionType useType, Bit32u useStartAddr) : MemoryRegion(useSynth, useRealMemory, NULL, useType, useStartAddr, D20_PATTERN_SIZE, 32) {}
};
class D20RhythmTrackMemoryRegion : public MemoryRegion {
public:
	D20RhythmTrackMemoryRegion(Synth *useSynth, Bit8u *useRealMemory) : MemoryRegion(useSynth, useRealMemory, NULL, MR_D20RhythmTrack, MT32EMU_MEMADDR(0x0C0000), D20_RHYTHM_TRACK_SIZE, 1) {}
};
// Fine pan of the parts (12 00 00) and of the rhythm keys (12 01 00), two bytes each; see MemParams::FinePan.
class PartFinePanMemoryRegion : public MemoryRegion {
public:
	PartFinePanMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_PartFinePan, MT32EMU_MEMADDR(0x120000), sizeof(MemParams::FinePan), MAX_PART_COUNT) {}
};
class RhythmFinePanMemoryRegion : public MemoryRegion {
public:
	RhythmFinePanMemoryRegion(Synth *useSynth, Bit8u *useRealMemory, Bit8u *useMaxTable) : MemoryRegion(useSynth, useRealMemory, useMaxTable, MR_RhythmFinePan, MT32EMU_MEMADDR(0x120100), sizeof(MemParams::FinePan), 85) {}
};
class DisplayMemoryRegion : public MemoryRegion {
public:
	DisplayMemoryRegion(Synth *useSynth) : MemoryRegion(useSynth, NULL, NULL, MR_Display, MT32EMU_MEMADDR(0x200000), SYSEX_BUFFER_SIZE - 1, 1) {}
};
class ResetMemoryRegion : public MemoryRegion {
public:
	ResetMemoryRegion(Synth *useSynth) : MemoryRegion(useSynth, NULL, NULL, MR_Reset, MT32EMU_MEMADDR(0x7F0000), 0x3FFF, 1) {}
};

} // namespace MT32Emu

#endif // #ifndef MT32EMU_MEMORY_REGION_H
