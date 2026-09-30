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

#ifndef MT32EMU_PART_H
#define MT32EMU_PART_H

#include "globals.h"
#include "internals.h"
#include "Types.h"
#include "Structures.h"

namespace MT32Emu {

class Poly;
class Synth;

class PolyList {
private:
	Poly *firstPoly;
	Poly *lastPoly;

public:
	PolyList();
	bool isEmpty() const;
	Poly *getFirst() const;
	Poly *getLast() const;
	void prepend(Poly *poly);
	void append(Poly *poly);
	Poly *takeFirst();
	void remove(Poly * const poly);
};

class Part {
private:
	// Direct pointer to sysex-addressable memory dedicated to this part (valid for parts 1-8, NULL for rhythm)
	TimbreParam *timbreTemp;

	// 0=Part 1, .. 7=Part 8, 8=Rhythm
	unsigned int partNum;

	bool holdpedal;

	unsigned int activePartialCount;
	PatchCache patchCache[4];
	PolyList activePolys;

	void setPatch(const PatchParam *patch);
	unsigned int midiKeyToKey(unsigned int midiKey);

	bool abortFirstPoly(unsigned int key);

protected:
	Synth *synth;
	// Direct pointer into sysex-addressable memory
	MemParams::PatchTemp *patchTemp;
	char name[8]; // "Part 1".."Part 8", "Rhythm"
	char currentInstr[11];
	Bit8u modulation;
	Bit8u expression;
	// D-110: MIDI volume (0-100) is a separate factor; the MT-32 writes it into the output level instead.
	Bit8u midiVolume;
	// Timbre number (0-127) from the last program change, 0xFF if none; lastProgramCard: from the memory card;
	// lastProgramAlt: one of the extra timbres D11-E88 (setAltProgram).
	Bit8u lastProgram;
	bool lastProgramCard;
	bool lastProgramAlt;
	Bit32s pitchBend;
	bool nrpn;
	Bit16u rpn;
	// Number of the last NRPN selected (MSB << 8 | LSB), 0x7F7F = none.
	Bit16u nrpnNumber;
	// MIDI extensions: offsets to the TVF cutoff (in cutoff parameter steps, applied continuously) and
	// resonance (applied to new notes), from GS/XG NRPNs 01H 20H/21H or CC 74/71.
	Bit8s cutoffOffset;
	Bit8s resonanceOffset;
	// MIDI extensions: offsets (-64..+63) to the TVA and TVF envelope times attack (T1), decay (T2-T4) and release (T5),
	// applied from the next envelope segment, from GS NRPNs 01H 63H/64H/66H or CC 73/75/72; and to the pitch LFO's rate
	// and depth, applied from its next half cycle, from GS NRPNs 01H 08H/09H or CC 76/77.
	Bit8s attackOffset;
	Bit8s decayOffset;
	Bit8s releaseOffset;
	Bit8s vibratoRateOffset;
	Bit8s vibratoDepthOffset;
	// MIDI extensions: the MIDI channel's pitch bend range (RPN 0), which program changes and timbre edits leave alone,
	// as on General MIDI modules: 2 semitones at power-on and after a reset.
	Bit8u bendRangeSemitones;
	Bit8u bendRangeCents;
	// (patchTemp->patch.benderRange * 683) at the time of the last MIDI program change or MIDI data entry; or, with the
	// MIDI extensions, the channel's range in the same units.
	Bit16u pitchBenderRange;
	// MIDI extensions: portamento, as GS modules have it. CC 65 switches it on (each note glides from the last note-on),
	// CC 5 sets its time (portamentoRate: key pitch in TVP's units, 16.16 fixed point, per tick of its 500 kHz timer),
	// and CC 84 names the key the next note glides from, or moves a note sounding on that key to the new one (legato).
	bool portamento;
	Bit8u portamentoTime;
	Bit32u portamentoRate;
	int portamentoControlKey; // MIDI key from CC 84, -1 = none
	int lastNoteKey; // MIDI key of the last note-on, -1 = none
	int portamentoSourceKey; // While a note-on starts its partials: the key they glide from (as midiKeyToKey gives it), or -1

	// CC 84's legato: a note sounding (not released) on key `fromKey` moves to `key`, gliding there. False if none sounds.
	bool legatoNote(unsigned int fromKey, unsigned int key);

	void backupCacheToPartials(PatchCache cache[4]);
	void cacheTimbre(PatchCache cache[4], const TimbreParam *timbre);
	void playPoly(const PatchCache cache[4], const MemParams::RhythmTemp *rhythmTemp, unsigned int midiKey, unsigned int key, unsigned int velocity);
	void stopNote(unsigned int key);
	const char *getName() const;

public:
	Part(Synth *synth, unsigned int usePartNum);
	virtual ~Part();
	void reset();
	void setDataEntryMSB(unsigned char midiDataEntryMSB);
	// MIDI extensions: the cents of the pitch bend range (RPN 0) while the channel's range is in use; ignored otherwise.
	void setDataEntryLSB(unsigned char midiDataEntryLSB);
	void setNRPNLSB(unsigned char midiNRPNLSB);
	void setNRPNMSB(unsigned char midiNRPNMSB);
	void setRPNLSB(unsigned char midiRPNLSB);
	void setRPNMSB(unsigned char midiRPNMSB);
	// Reset All Controllers (CC 121). The values the MIDI extensions set through RPNs, NRPNs and sound controllers
	// stay, as General MIDI (RP-015) and GS have it.
	void resetAllControllers();
	// Silences the part and resets everything a MIDI channel carries (controllers, pedal, bend, RPN/NRPN, and what
	// the MIDI extensions set: the pitch bend range and the sound controllers' offsets).
	void resetMIDIState();
	// Clears what the MIDI extensions keep per channel: the controller offsets, and the pitch bend range back to 2.
	void resetExtensionState();
	// MIDI extensions: CC 74 (brightness) and CC 71 (harmonic content), 64 = no change.
	void setBrightness(unsigned int midiValue);
	void setHarmonicContent(unsigned int midiValue);
	Bit8s getCutoffOffset() const;
	Bit8s getResonanceOffset() const;
	// MIDI extensions: CC 73 (attack time), 75 (decay time), 72 (release time), 76 (vibrato rate) and 77 (vibrato
	// depth), 64 = no change; the GS NRPNs for them set the same.
	void setAttackTime(unsigned int midiValue);
	void setDecayTime(unsigned int midiValue);
	void setReleaseTime(unsigned int midiValue);
	void setVibratoRate(unsigned int midiValue);
	void setVibratoDepth(unsigned int midiValue);
	// The time a TVA or TVF envelope takes for its time `index`: 0 (attack, T1), 1-3 (decay, T2-T4) or 4 (release, T5)
	// of `envTimes`, moved by the part's offset for it, within 0-100.
	int getEnvTime(const Bit8u *envTimes, unsigned int index) const;
	// The pitch LFO's rate and depth, moved by the part's offsets, within 0-100.
	int getLFORate(Bit8u rate) const;
	int getLFODepth(Bit8u depth) const;
	// The pitch bend range each way in cents: the channel's (RPN 0) with the MIDI extensions, else the timbre's.
	unsigned int getPitchBendRangeCents() const;
	// MIDI extensions: CC 65 (portamento switch, 64 and up = on), CC 5 (portamento time) and CC 84 (portamento control).
	void setPortamento(bool enabled);
	void setPortamentoTime(unsigned int midiValue);
	void setPortamentoControl(unsigned int midiKey);
	// For TVP: the key the partials being started glide from (-1: none), and the glide's rate (see portamentoRate).
	int getPortamentoSourceKey() const;
	Bit32u getPortamentoRate() const;
	virtual void noteOn(unsigned int midiKey, unsigned int velocity);
	virtual void noteOff(unsigned int midiKey);
	void allNotesOff();
	void allSoundOff();
	Bit8u getVolume() const; // Internal volume, 0-100, exposed for use by ExternalInterface
	void setVolume(unsigned int midiVolume);
	Bit8u getMIDIVolume() const; // D-110 MIDI volume 0-100 (always 100 for the MT-32 family)
	Bit8u getLastProgram() const;
	// D-110: true if this part's output assign is MULTI 5/6, which are unusable while reverb is on.
	bool isOutputMuted(Bit8u outputAssign) const;
	Bit8u getModulation() const;
	void setModulation(unsigned int midiModulation);
	Bit8u getExpression() const;
	void setExpression(unsigned int midiExpression);
	virtual void setPan(unsigned int midiPan);
	Bit32s getPitchBend() const;
	void setBend(unsigned int midiBend);
	virtual void setProgram(unsigned int midiProgram);
	// D-110: loads card timbre 0-127 (C-A11-C-B88) and switches tone group i/c to the card's tones.
	void setCardProgram(unsigned int cardTimbreNum);
	bool isLastProgramCard() const;
	// D-110 extension: loads timbre 0-127 of the extra banks D11-E88, which play tones d11-e88 (the extra tone banks)
	// with the settings of a new timbre memory (no key shift or fine tune, bender range 12, POLY 3, reverb).
	void setAltProgram(unsigned int altTimbreNum);
	bool isLastProgramAlt() const;
	void setHoldPedal(bool pedalval);
	void stopPedalHold();
	void updatePitchBenderRange();
	virtual void refresh();
	virtual void refreshTimbre(unsigned int absTimbreNum);
	virtual void setTimbre(TimbreParam *timbre);
	virtual unsigned int getAbsTimbreNum() const;
	const char *getCurrentInstr() const;
	const Poly *getFirstActivePoly() const;
	unsigned int getActivePartialCount() const;
	unsigned int getActiveNonReleasingPartialCount() const;
	Synth *getSynth() const;

	const MemParams::PatchTemp *getPatchTemp() const;

	// This should only be called by Poly
	void partialDeactivated(Poly *poly);

	// These are rather specialised, and should probably only be used by PartialManager
	bool abortFirstPoly(PolyState polyState);
	// Abort the first poly in PolyState_HELD, or if none exists, the first active poly in any state.
	bool abortFirstPolyPreferHeld();
	bool abortFirstPoly();
}; // class Part

class RhythmPart: public Part {
	// Pointer to the area of the MT-32's memory dedicated to rhythm
	const MemParams::RhythmTemp *rhythmTemp;

	// This caches the timbres/settings in use by the rhythm part
	PatchCache drumCache[85][4];
public:
	RhythmPart(Synth *synth, unsigned int usePartNum);
	void refresh();
	void refreshTimbre(unsigned int timbreNum);
	void setTimbre(TimbreParam *timbre);
	void noteOn(unsigned int key, unsigned int velocity);
	void noteOff(unsigned int midiKey);
	unsigned int getAbsTimbreNum() const;
	void setPan(unsigned int midiPan);
	void setProgram(unsigned int patchNum);
};

} // namespace MT32Emu

#endif // #ifndef MT32EMU_PART_H
