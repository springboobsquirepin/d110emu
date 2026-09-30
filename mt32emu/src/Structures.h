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

#ifndef MT32EMU_STRUCTURES_H
#define MT32EMU_STRUCTURES_H

#include "globals.h"
#include "Types.h"

namespace MT32Emu {

// MT32EMU_MEMADDR() converts from sysex-padded, MT32EMU_SYSEXMEMADDR converts to it
// Roland provides documentation using the sysex-padded addresses, so we tend to use that in code and output
#define MT32EMU_MEMADDR(x) ((((x) & 0x7f0000) >> 2) | (((x) & 0x7f00) >> 1) | ((x) & 0x7f))
#define MT32EMU_SYSEXMEMADDR(x) ((((x) & 0x1FC000) << 2) | (((x) & 0x3F80) << 1) | ((x) & 0x7f))

#ifdef _MSC_VER
#define  MT32EMU_ALIGN_PACKED __declspec(align(1))
#else
#define MT32EMU_ALIGN_PACKED __attribute__((packed))
#endif

// The following structures represent the MT-32's memory
// Since sysex allows this memory to be written to in blocks of bytes,
// we keep this packed so that we can copy data into the various
// banks directly
#if defined(_MSC_VER) || defined(__MINGW32__)
#pragma pack(push, 1)
#else
#pragma pack(1)
#endif

struct TimbreParam {
	struct CommonParam {
		char name[10];
		Bit8u partialStructure12;  // 1 & 2  0-12 (1-13)
		Bit8u partialStructure34;  // 3 & 4  0-12 (1-13)
		Bit8u partialMute;  // 0-15 (0000-1111)
		Bit8u noSustain; // ENV MODE 0-1 (Normal, No sustain)
	} MT32EMU_ALIGN_PACKED common;

	struct PartialParam {
		struct WGParam {
			Bit8u pitchCoarse;  // 0-96 (C1,C#1-C9)
			Bit8u pitchFine;  // 0-100 (-50 to +50 (cents - confirmed by Mok))
			Bit8u pitchKeyfollow;  // 0-16 (-1, -1/2, -1/4, 0, 1/8, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8, 1, 5/4, 3/2, 2, s1, s2)
			Bit8u pitchBenderEnabled;  // 0-1 (OFF, ON)
			Bit8u waveform; // MT-32: 0-1 (SQU/SAW); LAPC-I: WG WAVEFORM/PCM BANK 0 - 3 (SQU/1, SAW/1, SQU/2, SAW/2)
			Bit8u pcmWave; // 0-127 (1-128)
			Bit8u pulseWidth; // 0-100
			Bit8u pulseWidthVeloSensitivity; // 0-14 (-7 - +7)
		} MT32EMU_ALIGN_PACKED wg;

		struct PitchEnvParam {
			Bit8u depth; // 0-10
			Bit8u veloSensitivity; // 0-100
			Bit8u timeKeyfollow; // 0-4
			Bit8u time[4]; // 0-100
			Bit8u level[5]; // 0-100 (-50 - +50) // [3]: SUSTAIN LEVEL, [4]: END LEVEL
		} MT32EMU_ALIGN_PACKED pitchEnv;

		struct PitchLFOParam {
			Bit8u rate; // 0-100
			Bit8u depth; // 0-100
			Bit8u modSensitivity; // 0-100
		} MT32EMU_ALIGN_PACKED pitchLFO;

		struct TVFParam {
			Bit8u cutoff; // 0-100
			Bit8u resonance; // 0-30
			Bit8u keyfollow; // -1, -1/2, -1/4, 0, 1/8, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8, 1, 5/4, 3/2, 2
			Bit8u biasPoint; // 0-127 (<1A-<7C >1A-7C)
			Bit8u biasLevel; // 0-14 (-7 - +7)
			Bit8u envDepth; // 0-100
			Bit8u envVeloSensitivity; // 0-100
			Bit8u envDepthKeyfollow; // DEPTH KEY FOLL0W 0-4
			Bit8u envTimeKeyfollow; // TIME KEY FOLLOW 0-4
			Bit8u envTime[5]; // 0-100
			Bit8u envLevel[4]; // 0-100 // [3]: SUSTAIN LEVEL
		} MT32EMU_ALIGN_PACKED tvf;

		struct TVAParam {
			Bit8u level; // 0-100
			Bit8u veloSensitivity; // 0-100
			Bit8u biasPoint1; // 0-127 (<1A-<7C >1A-7C)
			Bit8u biasLevel1; // 0-12 (-12 - 0)
			Bit8u biasPoint2; // 0-127 (<1A-<7C >1A-7C)
			Bit8u biasLevel2; // 0-12 (-12 - 0)
			Bit8u envTimeKeyfollow; // TIME KEY FOLLOW 0-4
			Bit8u envTimeVeloSensitivity; // VELOS KEY FOLL0W 0-4
			Bit8u envTime[5]; // 0-100
			Bit8u envLevel[4]; // 0-100 // [3]: SUSTAIN LEVEL
		} MT32EMU_ALIGN_PACKED tva;
	} MT32EMU_ALIGN_PACKED partial[4]; // struct PartialParam
} MT32EMU_ALIGN_PACKED; // struct TimbreParam

struct PatchParam {
	Bit8u timbreGroup; // TIMBRE GROUP  0-3 (group A, group B, Memory, Rhythm)
	Bit8u timbreNum; // TIMBRE NUMBER 0-63
	Bit8u keyShift; // KEY SHIFT 0-48 (-24 - +24 semitones)
	Bit8u fineTune; // FINE TUNE 0-100 (-50 - +50 cents)
	Bit8u benderRange; // BENDER RANGE 0-24
	Bit8u assignMode;  // ASSIGN MODE 0-3 (POLY1, POLY2, POLY3, POLY4)
	Bit8u reverbSwitch;  // REVERB SWITCH 0-1 (OFF,ON)
	Bit8u dummy; // (DUMMY)
} MT32EMU_ALIGN_PACKED;

const unsigned int SYSTEM_MASTER_TUNE_OFF = 0;
const unsigned int SYSTEM_REVERB_MODE_OFF = 1;
const unsigned int SYSTEM_REVERB_TIME_OFF = 2;
const unsigned int SYSTEM_REVERB_LEVEL_OFF = 3;
const unsigned int SYSTEM_RESERVE_SETTINGS_START_OFF = 4;
const unsigned int SYSTEM_RESERVE_SETTINGS_END_OFF = 12;
const unsigned int SYSTEM_CHAN_ASSIGN_START_OFF = 13;
const unsigned int SYSTEM_CHAN_ASSIGN_END_OFF = 21;
const unsigned int SYSTEM_MASTER_VOL_OFF = 22;
// D-110 system area: 0x16 is a dummy (no master volume), 0x17-0x20 hold the current patch name.
const unsigned int SYSTEM_PATCH_NAME_OFF = 23;
const unsigned int SYSTEM_SIZE_MT32 = 23;
const unsigned int SYSTEM_SIZE_D110 = 33;
// A D-20 system area is 50 bytes: its 0x0D-0x20 are dummies (the D-110's channels and patch name),
// followed by part output levels (0x21-0x29) and pans (0x2A-0x31).
const unsigned int SYSTEM_D20_EXT_SIZE = 17;

// D-110 output assign, stored where the MT-32 keeps the reverb switch (patch byte 6, rhythm setup byte 3).
// 0 and 1 are compatible with the MT-32/D-20 reverb switch.
const Bit8u D110_OUTPUT_MIX_DRY = 0;
const Bit8u D110_OUTPUT_MIX_REVERB = 1;
const Bit8u D110_OUTPUT_MULTI_1 = 2; // 2-7: MULTI 1-6, dry and unpanned
const Bit8u D110_OUTPUT_MULTI_5 = 6;
const Bit8u D110_REVERB_MODE_OFF = 8;

// D-110 patch memory entry (06 00 00 + 01 00 * n): a complete multi-timbral setup.
struct D110PatchPartParam {
	Bit8u toneGroup; // TONE GROUP 0-4 (a, b, i, c, r)
	Bit8u toneNumber; // TONE NUMBER 0-63
	Bit8u keyShift; // KEY SHIFT 0-48 (-24 - +24)
	Bit8u fineTune; // FINE TUNE 0-100 (-50 - +50)
	Bit8u benderRange; // BENDER RANGE 0-24
	Bit8u assignMode; // ASSIGN MODE 0-3 (POLY1-POLY4)
	Bit8u outputAssign; // OUTPUT ASSIGN 0-7 (MIX, MIX+reverb, MULTI 1-6)
	Bit8u dummy; // PART_ALT_TONES: tone group a or b means d or e (an extension)
	Bit8u outputLevel; // OUTPUT LEVEL 0-100
	Bit8u panpot; // PANPOT 0-14 (L-R)
	Bit8u keyRangeLower; // KEY RANGE LOWER 0-127
	Bit8u keyRangeUpper; // KEY RANGE UPPER 0-127
} MT32EMU_ALIGN_PACKED;

struct D110PatchParam {
	char name[10]; // PATCH NAME
	Bit8u reverbMode; // REVERB MODE 0-8 (Room 1/2, Hall 1/2, Plate, Tap delay 1/2/3, OFF)
	Bit8u reverbTime; // REVERB TIME 0-7 (1-8)
	Bit8u reverbLevel; // REVERB LEVEL 0-7
	Bit8u reserveSettings[9]; // PARTIAL RESERVE (Part 1-8, R) 0-32
	Bit8u chanAssign[9]; // MIDI CHANNEL (Part 1-8, R) 0-16 (1-16, OFF)
	D110PatchPartParam parts[8];
	Bit8u rhythmOutputLevel; // OUTPUT LEVEL (Rhythm part) 0-100
} MT32EMU_ALIGN_PACKED;

// Tones of the D-110 memory card (group c) follow the rhythm tones in MemParams::timbres.
const unsigned int CARD_TONE_BASE = 256;
// Two more banks of 64 tones, d and e (an extension), follow the card's. The application fills them (Synth::setAltTimbre),
// e.g. with the MT-32's presets, or with the D-110's own a and b while those hold the MT-32's.
const unsigned int ALT_TONE_BASE = 320;
const unsigned int TIMBRE_COUNT = ALT_TONE_BASE + 128;
// A part's timbre temporary area uses tone group 2 for internal (i) or card (c) tones; the card ones when its dummy
// byte (offset 07H, which the D-110 ROM lets SysEx set to 0-127) holds this flag.
const Bit8u PART_CARD_TONES = 1;
// ...and tone groups 0 and 1 for the preset tones (a, b), or the extra banks (d, e) with this flag.
const Bit8u PART_ALT_TONES = 2;

// D-20 performance patch (patch memory 07 00 00 + 26H * n, patch temporary area 03 04 00).
const unsigned int D20_PATCH_SIZE = 38;
// D-20 rhythm patterns: 294 bytes each, sent as 588 nibbles (low nibble first); the rhythm track: 502 bytes.
const unsigned int D20_PATTERN_SIZE = 0x24C;
const unsigned int D20_RHYTHM_TRACK_SIZE = 0x1F6;

struct D20PatchParam {
	Bit8u keyMode; // KEY MODE 0-2 (whole, dual, split)
	Bit8u splitPoint; // SPLIT POINT 0-61 (C2-C#7): the upper tone starts at key 36 + splitPoint
	Bit8u lowerToneGroup; // LOWER TONE GROUP 0-3 (a, b, i, r)
	Bit8u lowerToneNumber; // LOWER TONE NUMBER 0-63
	Bit8u upperToneGroup; // UPPER TONE GROUP 0-3 (a, b, i, r)
	Bit8u upperToneNumber; // UPPER TONE NUMBER 0-63
	Bit8u lowerKeyShift; // LOWER KEY SHIFT 0-48 (-24 - +24)
	Bit8u upperKeyShift; // UPPER KEY SHIFT 0-48 (-24 - +24)
	Bit8u lowerFineTune; // LOWER FINE TUNE 0-100 (-50 - +50)
	Bit8u upperFineTune; // UPPER FINE TUNE 0-100 (-50 - +50)
	Bit8u lowerBenderRange; // LOWER BENDER RANGE 0-24
	Bit8u upperBenderRange; // UPPER BENDER RANGE 0-24
	Bit8u lowerAssignMode; // LOWER ASSIGN MODE 0-3 (POLY1-POLY4)
	Bit8u upperAssignMode; // UPPER ASSIGN MODE 0-3 (POLY1-POLY4)
	Bit8u lowerReverbSwitch; // LOWER REVERB SWITCH 0-1
	Bit8u upperReverbSwitch; // UPPER REVERB SWITCH 0-1
	Bit8u reverbMode; // REVERB MODE 0-8 (Room 1/2, Hall 1/2, Plate, Delay 1/2/3, OFF)
	Bit8u reverbTime; // REVERB TIME 0-7 (1-8)
	Bit8u reverbLevel; // REVERB LEVEL 0-7
	Bit8u balance; // U/L BALANCE 0-100 (lower max <-> upper max)
	Bit8u patchLevel; // PATCH LEVEL 0-100
	char name[16]; // PATCH NAME
	Bit8u dummy;
} MT32EMU_ALIGN_PACKED;

// Part numbers: 0-7 are parts 1-8, 8 is the rhythm part, and 9-15 are parts 9-15 of the 16-part mode (15 melodic
// parts and rhythm, an extension of the D-110 mode; their temporary areas and settings sit 10H above the D-110's).
const unsigned int RHYTHM_PART_NUM = 8;
const unsigned int BASE_PART_COUNT = 9;
const unsigned int MAX_PART_COUNT = 16;
// Part outputs (Synth::setPartOutputsAvailable): a stereo stream per part, then the D-110's MULTI outputs 1-6 in pairs
// (1+2, 3+4, 5+6) when they are outputs of their own (Synth::setMultiOutputsEnabled).
const unsigned int MULTI_OUTPUT_PAIRS = 3;
const unsigned int OUTPUT_STREAM_COUNT = MAX_PART_COUNT + MULTI_OUTPUT_PAIRS;
const unsigned int EXTRA_PART_COUNT = MAX_PART_COUNT - BASE_PART_COUNT;

struct MemParams {
	// NOTE: The MT-32 documentation only specifies PatchTemp areas for parts 1-8.
	// The LAPC-I documentation specified an additional area for rhythm at the end,
	// where all parameters but fine tune, assign mode and output level are ignored
	struct PatchTemp {
		PatchParam patch;
		Bit8u outputLevel; // OUTPUT LEVEL 0-100
		Bit8u panpot; // PANPOT 0-14 (R-L)
		Bit8u dummyv[6];
	} MT32EMU_ALIGN_PACKED patchTemp[MAX_PART_COUNT]; // Parts 1-8, rhythm, then parts 9-15 (16-part mode)

	struct RhythmTemp {
		Bit8u timbre; // TIMBRE  0-94 (M1-M64,R1-30,OFF); LAPC-I: 0-127 (M01-M64,R01-R63)
		Bit8u outputLevel; // OUTPUT LEVEL 0-100
		Bit8u panpot; // PANPOT 0-14 (R-L)
		Bit8u reverbSwitch;  // REVERB SWITCH 0-1 (OFF,ON)
	} MT32EMU_ALIGN_PACKED rhythmTemp[85];

	TimbreParam timbreTemp[MAX_PART_COUNT - 1]; // Parts 1-8, then parts 9-15 (16-part mode)

	PatchParam patches[128];

	// NOTE: There are only 30 timbres in the "rhythm" bank for MT-32; the additional 34 are for LAPC-I and above
	struct PaddedTimbre {
		TimbreParam timbre;
		Bit8u padding[10];
	} MT32EMU_ALIGN_PACKED timbres[TIMBRE_COUNT]; // Group A, Group B, Memory, Rhythm, Card (D-110 RAM card), d and e

	struct System {
		Bit8u masterTune; // MASTER TUNE 0-127 432.1-457.6Hz
		Bit8u reverbMode; // REVERB MODE 0-3 (room, hall, plate, tap delay)
		Bit8u reverbTime; // REVERB TIME 0-7 (1-8)
		Bit8u reverbLevel; // REVERB LEVEL 0-7 (1-8)
		Bit8u reserveSettings[9]; // PARTIAL RESERVE (PART 1) 0-32
		Bit8u chanAssign[9]; // MIDI CHANNEL (PART1) 0-16 (1-16,OFF)
		Bit8u masterVol; // MASTER VOLUME 0-100 (D-110: dummy, kept at 100)
		char patchName[10]; // D-110 only: PATCH NAME of the current patch
	} MT32EMU_ALIGN_PACKED system;

	// 16-part mode (11 00 00): partial reserves (0-32) of parts 9-15 at 00-06 and their MIDI channels (0-16) at 08-0E.
	// 07 and 0F are unused (a 16th melodic part once sat there).
	struct SystemExt {
		Bit8u reserveSettings[EXTRA_PART_COUNT];
		Bit8u unused1;
		Bit8u chanAssign[EXTRA_PART_COUNT];
		Bit8u unused2;
	} MT32EMU_ALIGN_PACKED systemExt;

	// D-110 memory card (M-256D): card tones c11-c88 are timbres[CARD_TONE_BASE...]; card timbres C-A11-C-B88 and
	// card patches C-11-C-88 are here. SysEx reaches them at 18 00 00, 15 00 00 and 16 00 00 (an extension).
	PatchParam cardTimbres[128];
	D110PatchParam cardPatches[64];

	// D-110 only
	D110PatchParam d110Patches[64];
	D20PatchParam d20Patches[128];
	D20PatchParam d20PatchTemp; // The current performance patch (D-20 performance mode)

	// Fine pan (an extension, used with nice panning): per part at 12 00 00 (by part number) and per rhythm key at
	// 12 01 00 (keys 24-108). The value, 7-bit MSB then LSB, is 0 to follow the panpot, or 1-129 for -64..+64 (hard
	// left to hard right, as the D-110 shows pan). A fine pan moves the panpot to its nearest step; writing the
	// panpot drops the fine pan.
	struct FinePan {
		Bit8u msb;
		Bit8u lsb;
	} MT32EMU_ALIGN_PACKED partFinePan[MAX_PART_COUNT], rhythmFinePan[85];

	// D-20 rhythm data, kept from D-20 dumps (the D-110 has no rhythm machine; the application plays them): the
	// patterns P-11-P-48 (the D-20's presets, which are in its ROM; an extension at 0D 00 00) and P-51-P-88 (0A 00 00,
	// as on the D-20) as they are sent, in nibbles, and the rhythm track (0C 00 00).
	Bit8u d20Patterns[64][D20_PATTERN_SIZE];
	Bit8u d20RhythmTrack[D20_RHYTHM_TRACK_SIZE];
}; // struct MemParams

struct SoundGroup {
	Bit8u timbreNumberTableAddrLow;
	Bit8u timbreNumberTableAddrHigh;
	Bit8u displayPosition;
	Bit8u name[9];
	Bit8u timbreCount;
	Bit8u pad;
} MT32EMU_ALIGN_PACKED;

#if defined(_MSC_VER) || defined(__MINGW32__)
#pragma pack(pop)
#else
#pragma pack()
#endif

struct ControlROMFeatureSet {
	unsigned int quirkBasePitchOverflow : 1;
	unsigned int quirkPitchEnvelopeOverflow : 1;
	unsigned int quirkRingModulationNoMix : 1;
	unsigned int quirkTVAZeroEnvLevels : 1;
	unsigned int quirkPanMult : 1;
	unsigned int quirkKeyShift : 1;
	unsigned int quirkTVFBaseCutoffLimit : 1;

	// Features below don't actually depend on control ROM version, which is used to identify hardware model
	unsigned int defaultReverbMT32Compatible : 1;
	unsigned int oldMT32AnalogLPF : 1;
	// D-110 powers up with Parts 1-8 on MIDI channels 1-8 (MT-32 family: 2-9); Rhythm is on channel 10 in both cases.
	unsigned int defaultChannelsStartAt1 : 1;
	// D-110 panpot 0-14 runs left to right; the MT-32 family's runs right to left. Applies to SysEx and MIDI pan.
	unsigned int panLeftToRight : 1;
	// D-110 address map and behaviour: output assign instead of reverb switch, part key ranges,
	// MIDI volume separate from output level, 9 reverb types, patch memory, write requests,
	// program changes on the control channel select patches.
	unsigned int d110MemoryMap : 1;
};

struct ControlROMMap {
	const char *shortName;
	const ControlROMFeatureSet &featureSet;
	Bit16u pcmTable; // 4 * pcmCount bytes
	Bit16u pcmCount;
	Bit16u timbreAMap; // 128 bytes
	Bit16u timbreAOffset;
	bool timbreACompressed;
	Bit16u timbreBMap; // 128 bytes
	Bit16u timbreBOffset;
	bool timbreBCompressed;
	Bit16u timbreRMap; // 2 * timbreRCount bytes
	Bit16u timbreRCount;
	Bit16u rhythmSettings; // 4 * rhythmSettingsCount bytes
	Bit16u rhythmSettingsCount;
	Bit16u reserveSettings; // 9 bytes
	Bit16u panSettings; // 8 bytes
	Bit16u programSettings; // 8 bytes
	Bit16u rhythmMaxTable; // 4 bytes
	Bit16u patchMaxTable; // 16 bytes
	Bit16u systemMaxTable; // 23 bytes
	Bit16u timbreMaxTable; // 72 bytes
	Bit16u soundGroupsTable; // 14 bytes each entry
	Bit16u soundGroupsCount;
};

struct ControlROMPCMStruct {
	Bit8u pos;
	Bit8u len;
	Bit8u pitchLSB;
	Bit8u pitchMSB;
};

struct PCMWaveEntry {
	Bit32u addr;
	Bit32u len;
	bool loop;
	ControlROMPCMStruct *controlROMPCMStruct;
};

// This is basically a per-partial, pre-processed combination of timbre and patch/rhythm settings
struct PatchCache {
	bool playPartial;
	bool PCMPartial;
	int pcm;
	Bit8u waveform;

	Bit32u structureMix;
	int structurePosition;
	int structurePair;

	// The following fields are actually common to all partials in the timbre
	bool dirty;
	Bit32u partialCount;
	bool sustain;
	bool reverb;

	TimbreParam::PartialParam srcPartial;

	// The following directly points into live sysex-addressable memory
	const TimbreParam::PartialParam *partialParam;
};

} // namespace MT32Emu

#endif // #ifndef MT32EMU_STRUCTURES_H
