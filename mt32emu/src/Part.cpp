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

#include <cstdio>
#include <cstring>

#include "internals.h"

#include "Part.h"
#include "Partial.h"
#include "PartialManager.h"
#include "Poly.h"
#include "Synth.h"

namespace MT32Emu {

static const Bit8u PartialStruct[13] = {
	0, 0, 2, 2, 1, 3,
	3, 0, 3, 0, 2, 1, 3
};

static const Bit8u PartialMixStruct[13] = {
	0, 1, 0, 1, 1, 0,
	1, 3, 3, 2, 2, 2, 2
};

RhythmPart::RhythmPart(Synth *useSynth, unsigned int usePartNum): Part(useSynth, usePartNum) {
	strcpy(name, "Rhythm");
	rhythmTemp = &synth->mt32ram.rhythmTemp[0];
	refresh();
}

// MIDI extensions: the portamento time (CC 5) as a glide rate. TiMidity++'s curve for GS modules: 312.5 / 2^(time / 16)
// semitones a second (an octave in 38 ms at 0, 0.61 s at 64, 9.4 s at 127), as key pitch (4096 an octave) per tick of
// TVP's 500 kHz timer, 16.16 fixed point: 13981 at time 0, halving every 16 steps.
static Bit32u portamentoRateFor(unsigned int time) {
	static const Bit16u RATES[16] = {13981, 13388, 12821, 12277, 11757, 11258, 10781, 10324, 9886, 9467, 9066, 8681, 8313, 7961, 7623, 7300};
	if (time > 127) time = 127;
	return Bit32u(RATES[time & 15]) >> (time >> 4);
}

Part::Part(Synth *useSynth, unsigned int usePartNum) {
	synth = useSynth;
	partNum = usePartNum;
	patchCache[0].dirty = true;
	holdpedal = false;
	patchTemp = &synth->mt32ram.patchTemp[partNum];
	if (usePartNum == RHYTHM_PART_NUM) {
		// Nasty hack for rhythm
		timbreTemp = NULL;
	} else if (usePartNum < RHYTHM_PART_NUM) {
		snprintf(name, sizeof(name), "Part %d", partNum + 1);
		timbreTemp = &synth->mt32ram.timbreTemp[partNum];
	} else {
		// 16-part mode: part numbers 9-15 are parts 9-15, with tone temps after those of parts 1-8
		snprintf(name, sizeof(name), "Part %d", partNum);
		timbreTemp = &synth->mt32ram.timbreTemp[partNum - 1];
	}
	currentInstr[0] = 0;
	currentInstr[10] = 0;
	modulation = 0;
	expression = 100;
	midiVolume = 100;
	lastProgram = 0xFF;
	lastProgramCard = false;
	lastProgramAlt = false;
	pitchBend = 0;
	nrpn = false;
	rpn = 0xFFFF;
	nrpnNumber = 0x7F7F;
	cutoffOffset = 0;
	resonanceOffset = 0;
	attackOffset = 0;
	decayOffset = 0;
	releaseOffset = 0;
	vibratoRateOffset = 0;
	vibratoDepthOffset = 0;
	bendRangeSemitones = 2;
	bendRangeCents = 0;
	pitchBenderRange = 0;
	portamento = false;
	portamentoTime = 0;
	portamentoRate = portamentoRateFor(0);
	portamentoControlKey = -1;
	lastNoteKey = -1;
	portamentoSourceKey = -1;
	activePartialCount = 0;
	memset(patchCache, 0, sizeof(patchCache));
}

Part::~Part() {
	while (!activePolys.isEmpty()) {
		delete activePolys.takeFirst();
	}
}

void Part::setDataEntryMSB(unsigned char midiDataEntryMSB) {
	if (nrpn) {
		// The last RPN-related control change was for an NRPN, which the real synths don't support.
		// As an extension, the GS sound NRPNs are understood (the SC-88Pro's: relative changes, 40H = none).
		if (!synth->isMIDIExtensionsEnabled()) return;
		switch (nrpnNumber) {
		case 0x0108: setVibratoRate(midiDataEntryMSB); break;
		case 0x0109: setVibratoDepth(midiDataEntryMSB); break;
		case 0x0120: setBrightness(midiDataEntryMSB); break;
		case 0x0121: setHarmonicContent(midiDataEntryMSB); break;
		case 0x0163: setAttackTime(midiDataEntryMSB); break;
		case 0x0164: setDecayTime(midiDataEntryMSB); break;
		case 0x0166: setReleaseTime(midiDataEntryMSB); break;
		default: break;
		}
		return;
	}
	if (rpn == 0) {
		// Pitch bend sensitivity, the only RPN that these synths support
		if (synth->isMIDIExtensionsEnabled()) {
			// The channel's range, as on GM modules: semitones (up to 24), and a new MSB clears the cents.
			bendRangeSemitones = midiDataEntryMSB > 24 ? 24 : midiDataEntryMSB;
			bendRangeCents = 0;
		} else {
			patchTemp->patch.benderRange = midiDataEntryMSB > 24 ? 24 : midiDataEntryMSB;
		}
		updatePitchBenderRange();
		return;
	}
	if (!synth->isMIDIExtensionsEnabled()) return;
	if (rpn == 1) {
		// Extension: channel fine tuning (64 = 0, 100 cents per 64 steps), limited to the part's +/-50 cents.
		int cents = (int(midiDataEntryMSB) - 64) * 100 / 64;
		cents = cents < -50 ? -50 : (cents > 50 ? 50 : cents);
		patchTemp->patch.fineTune = Bit8u(50 + cents);
	} else if (rpn == 2) {
		// Extension: channel coarse tuning in semitones (64 = 0), limited to the part's +/-24 key shift.
		int semitones = int(midiDataEntryMSB) - 64;
		semitones = semitones < -24 ? -24 : (semitones > 24 ? 24 : semitones);
		patchTemp->patch.keyShift = Bit8u(24 + semitones);
	}
}

void Part::setDataEntryLSB(unsigned char midiDataEntryLSB) {
	if (nrpn || rpn != 0 || !synth->isMIDIExtensionsEnabled()) return;
	bendRangeCents = midiDataEntryLSB > 99 ? 99 : midiDataEntryLSB;
	updatePitchBenderRange();
}

void Part::setNRPNLSB(unsigned char midiNRPNLSB) {
	nrpn = true;
	nrpnNumber = Bit16u((nrpnNumber & 0xFF00) | midiNRPNLSB);
}

void Part::setNRPNMSB(unsigned char midiNRPNMSB) {
	nrpn = true;
	nrpnNumber = Bit16u((nrpnNumber & 0x00FF) | (midiNRPNMSB << 8));
}

void Part::setBrightness(unsigned int midiValue) {
	cutoffOffset = Bit8s(int(midiValue > 127 ? 127 : midiValue) - 64);
}

void Part::setHarmonicContent(unsigned int midiValue) {
	// Resonance spans 0-30, so a quarter of the controller range is plenty.
	resonanceOffset = Bit8s((int(midiValue > 127 ? 127 : midiValue) - 64) / 4);
}

Bit8s Part::getCutoffOffset() const {
	return cutoffOffset;
}

Bit8s Part::getResonanceOffset() const {
	return resonanceOffset;
}

// A sound controller's value (64 = no change) as an offset to a parameter of 0-100.
static Bit8s soundControllerOffset(unsigned int midiValue) {
	return Bit8s(int(midiValue > 127 ? 127 : midiValue) - 64);
}

void Part::setAttackTime(unsigned int midiValue) {
	attackOffset = soundControllerOffset(midiValue);
}

void Part::setDecayTime(unsigned int midiValue) {
	decayOffset = soundControllerOffset(midiValue);
}

void Part::setReleaseTime(unsigned int midiValue) {
	releaseOffset = soundControllerOffset(midiValue);
}

void Part::setVibratoRate(unsigned int midiValue) {
	// Eight steps of the LFO rate double it: half the offset spans 1/16 to 16 times the tone's own rate.
	vibratoRateOffset = Bit8s(soundControllerOffset(midiValue) / 2);
}

void Part::setVibratoDepth(unsigned int midiValue) {
	vibratoDepthOffset = soundControllerOffset(midiValue);
}

static int offsetParameter(int value, int offset) {
	if (offset == 0) return value; // Untouched, whatever its range
	value += offset;
	return value < 0 ? 0 : (value > 100 ? 100 : value);
}

int Part::getEnvTime(const Bit8u *envTimes, unsigned int index) const {
	return offsetParameter(envTimes[index], index == 0 ? attackOffset : (index == 4 ? releaseOffset : decayOffset));
}

int Part::getLFORate(Bit8u rate) const {
	return offsetParameter(rate, vibratoRateOffset);
}

int Part::getLFODepth(Bit8u depth) const {
	return offsetParameter(depth, vibratoDepthOffset);
}

unsigned int Part::getPitchBendRangeCents() const {
	if (synth->isMIDIExtensionsEnabled()) return bendRangeSemitones * 100U + bendRangeCents;
	return patchTemp->patch.benderRange * 100U;
}

void Part::setPortamento(bool enabled) {
	portamento = enabled;
}

void Part::setPortamentoTime(unsigned int midiValue) {
	portamentoTime = Bit8u(midiValue > 127 ? 127 : midiValue);
	portamentoRate = portamentoRateFor(portamentoTime);
}

void Part::setPortamentoControl(unsigned int midiKey) {
	portamentoControlKey = int(midiKey > 127 ? 127 : midiKey);
}

int Part::getPortamentoSourceKey() const {
	return portamentoSourceKey;
}

Bit32u Part::getPortamentoRate() const {
	return portamentoRate;
}

bool Part::legatoNote(unsigned int fromKey, unsigned int key) {
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		const PolyState state = poly->getState();
		if (poly->getKey() == fromKey && (state == POLY_Playing || state == POLY_Held)) {
			poly->legato(key);
			synth->reportHandler->onPolyStateChanged(Bit8u(partNum));
			return true;
		}
	}
	return false;
}

void Part::setRPNLSB(unsigned char midiRPNLSB) {
	nrpn = false;
	rpn = (rpn & 0xFF00) | midiRPNLSB;
}

void Part::setRPNMSB(unsigned char midiRPNMSB) {
	nrpn = false;
	rpn = (rpn & 0x00FF) | (midiRPNMSB << 8);
}

void Part::setHoldPedal(bool pressed) {
	if (holdpedal && !pressed) {
		holdpedal = false;
		stopPedalHold();
	} else {
		holdpedal = pressed;
	}
}

Bit32s Part::getPitchBend() const {
	return pitchBend;
}

void Part::setBend(unsigned int midiBend) {
	// CONFIRMED:
	pitchBend = ((signed(midiBend) - 8192) * pitchBenderRange) >> 14; // PORTABILITY NOTE: Assumes arithmetic shift
}

Bit8u Part::getModulation() const {
	return modulation;
}

void Part::setModulation(unsigned int midiModulation) {
	modulation = Bit8u(midiModulation);
}

void Part::resetAllControllers() {
	modulation = 0;
	expression = 100;
	// D-110: Reset All Controllers sets Main Volume to maximum. It stays 100 for the MT-32 family.
	midiVolume = 100;
	pitchBend = 0;
	setHoldPedal(false);
	// MIDI extensions: portamento goes off, as on GM and GS modules (its time stays).
	portamento = false;
	portamentoControlKey = -1;
}

void Part::resetExtensionState() {
	cutoffOffset = 0;
	resonanceOffset = 0;
	attackOffset = 0;
	decayOffset = 0;
	releaseOffset = 0;
	vibratoRateOffset = 0;
	vibratoDepthOffset = 0;
	bendRangeSemitones = 2;
	bendRangeCents = 0;
	updatePitchBenderRange();
	portamento = false;
	setPortamentoTime(0);
	portamentoControlKey = -1;
	lastNoteKey = -1;
}

void Part::reset() {
	resetAllControllers();
	resetExtensionState();
	allSoundOff();
	rpn = 0xFFFF;
}

void Part::resetMIDIState() {
	resetAllControllers();
	resetExtensionState();
	allSoundOff();
	nrpn = false;
	rpn = 0xFFFF;
	nrpnNumber = 0x7F7F;
}

void RhythmPart::refresh() {
	// (Re-)cache all the mapped timbres ahead of time
	for (unsigned int drumNum = 0; drumNum < synth->controlROMMap->rhythmSettingsCount; drumNum++) {
		int drumTimbreNum = rhythmTemp[drumNum].timbre;
		if (drumTimbreNum >= 127) { // 94 on MT-32
			continue;
		}
		PatchCache *cache = drumCache[drumNum];
		backupCacheToPartials(cache);
		const Bit8u reverbSwitch = rhythmTemp[drumNum].reverbSwitch;
		for (int t = 0; t < 4; t++) {
			// Common parameters, stored redundantly
			cache[t].dirty = true;
			// D-110: this byte is the output assign, and only "MIX with reverb" feeds the reverb.
			cache[t].reverb = synth->controlROMFeatures->d110MemoryMap ? reverbSwitch == D110_OUTPUT_MIX_REVERB : reverbSwitch > 0;
		}
	}
	updatePitchBenderRange();
}

void Part::refresh() {
	backupCacheToPartials(patchCache);
	const Bit8u reverbSwitch = patchTemp->patch.reverbSwitch;
	for (int t = 0; t < 4; t++) {
		// Common parameters, stored redundantly
		patchCache[t].dirty = true;
		// D-110: this byte is the output assign, and only "MIX with reverb" feeds the reverb.
		patchCache[t].reverb = synth->controlROMFeatures->d110MemoryMap ? reverbSwitch == D110_OUTPUT_MIX_REVERB : reverbSwitch > 0;
	}
	memcpy(currentInstr, timbreTemp->common.name, 10);
	synth->newTimbreSet(partNum, patchTemp->patch.timbreGroup, patchTemp->patch.timbreNum, currentInstr);
	updatePitchBenderRange();
}

const char *Part::getCurrentInstr() const {
	return &currentInstr[0];
}

void RhythmPart::refreshTimbre(unsigned int absTimbreNum) {
	for (int m = 0; m < 85; m++) {
		if (rhythmTemp[m].timbre == absTimbreNum - 128) {
			drumCache[m][0].dirty = true;
		}
	}
}

void Part::refreshTimbre(unsigned int absTimbreNum) {
	if (getAbsTimbreNum() == absTimbreNum) {
		memcpy(currentInstr, timbreTemp->common.name, 10);
		patchCache[0].dirty = true;
	}
}

void Part::setPatch(const PatchParam *patch) {
	patchTemp->patch = *patch;
}

void RhythmPart::setTimbre(TimbreParam * /*timbre*/) {
	synth->printDebug("%s: Attempted to call setTimbre() - doesn't make sense for rhythm", name);
}

void Part::setTimbre(TimbreParam *timbre) {
	*timbreTemp = *timbre;
}

unsigned int RhythmPart::getAbsTimbreNum() const {
	synth->printDebug("%s: Attempted to call getAbsTimbreNum() - doesn't make sense for rhythm", name);
	return 0;
}

unsigned int Part::getAbsTimbreNum() const {
	// D-110: tone group 2 is the internal tones (i), or the memory card's (c) when the part's card flag is set; groups
	// 0 and 1 are the preset tones (a, b), or the extra banks (d, e) with the alt flag while those are loaded.
	const Bit8u group = patchTemp->patch.timbreGroup;
	if (synth->isD110()) {
		if (group == 2 && patchTemp->patch.dummy == PART_CARD_TONES) {
			return CARD_TONE_BASE + (patchTemp->patch.timbreNum & 63);
		}
		if (group < 2 && patchTemp->patch.dummy == PART_ALT_TONES && synth->hasAltTimbres()) {
			return ALT_TONE_BASE + group * 64 + (patchTemp->patch.timbreNum & 63);
		}
	}
	return (group * 64) + patchTemp->patch.timbreNum;
}

#if MT32EMU_MONITOR_MIDI > 0
void RhythmPart::setProgram(unsigned int patchNum) {
	synth->printDebug("%s: Attempt to set program (%d) on rhythm is invalid", name, patchNum);
}
#else
void RhythmPart::setProgram(unsigned int) { }
#endif

void Part::setProgram(unsigned int patchNum) {
	lastProgram = Bit8u(patchNum);
	lastProgramCard = false;
	lastProgramAlt = false;
	setPatch(&synth->mt32ram.patches[patchNum]);
	holdpedal = false;
	allSoundOff();
	setTimbre(&synth->mt32ram.timbres[getAbsTimbreNum()].timbre);
	refresh();
}

void Part::setCardProgram(unsigned int cardTimbreNum) {
	lastProgram = Bit8u(cardTimbreNum & 0x7F);
	lastProgramCard = true;
	lastProgramAlt = false;
	setPatch(&synth->mt32ram.cardTimbres[cardTimbreNum & 0x7F]);
	// Tone group i is the card's; a timbre that plays d or e keeps them.
	const bool alt = patchTemp->patch.timbreGroup < 2 && patchTemp->patch.dummy == PART_ALT_TONES;
	patchTemp->patch.dummy = alt ? PART_ALT_TONES : PART_CARD_TONES;
	holdpedal = false;
	allSoundOff();
	setTimbre(&synth->mt32ram.timbres[getAbsTimbreNum()].timbre);
	refresh();
}

bool Part::isLastProgramCard() const {
	return lastProgramCard;
}

void Part::setAltProgram(unsigned int altTimbreNum) {
	lastProgram = Bit8u(altTimbreNum & 0x7F);
	lastProgramCard = false;
	lastProgramAlt = true;
	PatchParam patch;
	patch.timbreGroup = Bit8u((altTimbreNum & 0x7F) / 64);
	patch.timbreNum = Bit8u(altTimbreNum % 64);
	patch.keyShift = 24;
	patch.fineTune = 50;
	patch.benderRange = 12;
	patch.assignMode = 2;
	patch.reverbSwitch = 1;
	patch.dummy = PART_ALT_TONES;
	setPatch(&patch);
	holdpedal = false;
	allSoundOff();
	setTimbre(&synth->mt32ram.timbres[getAbsTimbreNum()].timbre);
	refresh();
}

bool Part::isLastProgramAlt() const {
	return lastProgramAlt;
}

void Part::updatePitchBenderRange() {
	if (synth->isMIDIExtensionsEnabled()) {
		pitchBenderRange = Bit16u(bendRangeSemitones * 683 + (bendRangeCents * 683 + 50) / 100);
	} else {
		pitchBenderRange = patchTemp->patch.benderRange * 683;
	}
}

void Part::backupCacheToPartials(PatchCache cache[4]) {
	// check if any partials are still playing with the old patch cache
	// if so then duplicate the cached data from the part to the partial so that
	// we can change the part's cache without affecting the partial.
	// We delay this until now to avoid a copy operation with every note played
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		poly->backupCacheToPartials(cache);
	}
}

void Part::cacheTimbre(PatchCache cache[4], const TimbreParam *timbre) {
	backupCacheToPartials(cache);
	int partialCount = 0;
	for (int t = 0; t < 4; t++) {
		if (((timbre->common.partialMute >> t) & 0x1) == 1) {
			cache[t].playPartial = true;
			partialCount++;
		} else {
			cache[t].playPartial = false;
			continue;
		}

		// Calculate and cache common parameters
		cache[t].srcPartial = timbre->partial[t];

		cache[t].pcm = timbre->partial[t].wg.pcmWave;

		switch (t) {
		case 0:
			cache[t].PCMPartial = (PartialStruct[int(timbre->common.partialStructure12)] & 0x2) ? true : false;
			cache[t].structureMix = PartialMixStruct[int(timbre->common.partialStructure12)];
			cache[t].structurePosition = 0;
			cache[t].structurePair = 1;
			break;
		case 1:
			cache[t].PCMPartial = (PartialStruct[int(timbre->common.partialStructure12)] & 0x1) ? true : false;
			cache[t].structureMix = PartialMixStruct[int(timbre->common.partialStructure12)];
			cache[t].structurePosition = 1;
			cache[t].structurePair = 0;
			break;
		case 2:
			cache[t].PCMPartial = (PartialStruct[int(timbre->common.partialStructure34)] & 0x2) ? true : false;
			cache[t].structureMix = PartialMixStruct[int(timbre->common.partialStructure34)];
			cache[t].structurePosition = 0;
			cache[t].structurePair = 3;
			break;
		case 3:
			cache[t].PCMPartial = (PartialStruct[int(timbre->common.partialStructure34)] & 0x1) ? true : false;
			cache[t].structureMix = PartialMixStruct[int(timbre->common.partialStructure34)];
			cache[t].structurePosition = 1;
			cache[t].structurePair = 2;
			break;
		default:
			break;
		}

		cache[t].partialParam = &timbre->partial[t];

		cache[t].waveform = timbre->partial[t].wg.waveform;
	}
	for (int t = 0; t < 4; t++) {
		// Common parameters, stored redundantly
		cache[t].dirty = false;
		cache[t].partialCount = partialCount;
		cache[t].sustain = (timbre->common.noSustain == 0);
	}
	//synth->printDebug("Res 1: %d 2: %d 3: %d 4: %d", cache[0].waveform, cache[1].waveform, cache[2].waveform, cache[3].waveform);

#if MT32EMU_MONITOR_INSTRUMENTS > 0
	synth->printDebug("%s (%s): Recached timbre", name, currentInstr);
	for (int i = 0; i < 4; i++) {
		synth->printDebug(" %d: play=%s, pcm=%s (%d), wave=%d", i, cache[i].playPartial ? "YES" : "NO", cache[i].PCMPartial ? "YES" : "NO", timbre->partial[i].wg.pcmWave, timbre->partial[i].wg.waveform);
	}
#endif
}

const char *Part::getName() const {
	return name;
}

void Part::setVolume(unsigned int newMidiVolume) {
	if (synth->controlROMFeatures->d110MemoryMap) {
		// D-110: Main Volume scales the part together with Output Level and Expression.
		midiVolume = Bit8u(newMidiVolume * 100 / 127);
		return;
	}
	// CONFIRMED: This calculation matches the table used in the control ROM
	patchTemp->outputLevel = Bit8u(newMidiVolume * 100 / 127);
	//synth->printDebug("%s (%s): Set volume to %d", name, currentInstr, midiVolume);
}

Bit8u Part::getVolume() const {
	return patchTemp->outputLevel;
}

Bit8u Part::getMIDIVolume() const {
	return midiVolume;
}

Bit8u Part::getLastProgram() const {
	return lastProgram;
}

bool Part::isOutputMuted(Bit8u outputAssign) const {
	// Reverb uses the DAC channels of MULTI outputs 5 and 6, so anything routed there is lost while reverb is on; not
	// on a part's own output, nor where the MULTI outputs are outputs of their own (setMultiOutputsEnabled).
	return synth->controlROMFeatures->d110MemoryMap && outputAssign >= D110_OUTPUT_MULTI_5
		&& synth->mt32ram.system.reverbMode != D110_REVERB_MODE_OFF && !synth->isPartOutputRouted(partNum)
		&& !synth->areMultiOutputsEnabled();
}

Bit8u Part::getExpression() const {
	return expression;
}

void Part::setExpression(unsigned int midiExpression) {
	// CONFIRMED: This calculation matches the table used in the control ROM
	expression = Bit8u(midiExpression * 100 / 127);
}

void RhythmPart::setPan(unsigned int midiPan) {
	// CONFIRMED: This does change patchTemp, but has no actual effect on playback.
#if MT32EMU_MONITOR_MIDI > 0
	synth->printDebug("%s: Pointlessly setting pan (%d) on rhythm part", name, midiPan);
#endif
	Part::setPan(midiPan);
}

void Part::setPan(unsigned int midiPan) {
	// NOTE: Panning is inverted compared to GM.

	if (synth->controlROMFeatures->quirkPanMult) {
		// MT-32: Divide by 9
		patchTemp->panpot = Bit8u(midiPan / 9);
	} else {
		// CM-32L: Divide by 8.5
		patchTemp->panpot = Bit8u((midiPan << 3) / 68);
	}
	// The fine pan keeps the controller's resolution for nice panning.
	synth->setFinePanFromMIDI(partNum, midiPan);

	//synth->printDebug("%s (%s): Set pan to %d", name, currentInstr, panpot);
}

/**
 * Applies key shift to a MIDI key and converts it into an internal key value in the range 12-108.
 */
unsigned int Part::midiKeyToKey(unsigned int midiKey) {
	if (synth->controlROMFeatures->quirkKeyShift) {
		// NOTE: On MT-32 GEN0, key isn't adjusted, and keyShift is applied further in TVP, unlike newer units:
		return midiKey;
	}
	int key = midiKey + patchTemp->patch.keyShift;
	if (key < 36) {
		// After keyShift is applied, key < 36, so move up by octaves
		while (key < 36) {
			key += 12;
		}
	} else if (key > 132) {
		// After keyShift is applied, key > 132, so move down by octaves
		while (key > 132) {
			key -= 12;
		}
	}
	key -= 24;
	return key;
}

void RhythmPart::noteOn(unsigned int midiKey, unsigned int velocity) {
	if (midiKey < 24 || midiKey > 108) { /*> 87 on MT-32)*/
		synth->printDebug("%s: Attempted to play invalid key %d (velocity %d)", name, midiKey, velocity);
		return;
	}
	unsigned int key = midiKey;
	unsigned int drumNum = key - 24;
	int drumTimbreNum = rhythmTemp[drumNum].timbre;
	const int drumTimbreCount = 64 + synth->controlROMMap->timbreRCount; // 94 on MT-32, 128 on LAPC-I/CM32-L
	if (drumTimbreNum == 127 || drumTimbreNum >= drumTimbreCount) { // timbre #127 is OFF, no sense to play it
		synth->printDebug("%s: Attempted to play unmapped key %d (velocity %d)", name, midiKey, velocity);
		return;
	}
	if (synth->isD110Mode()) {
		// D-110 firmware (rhythm note-on at 25A2H): the closed hi-hats r01/r02 end whatever plays on key 0 and
		// sound on key 1; Open High Hat-1 (r03) ends the previous one and sounds on key 0, so a closed hi-hat cuts
		// it off. Open High Hat-2 (r04) rings on like any other tone. The tones count, not the keys they are on.
		const int rhythmTone = drumTimbreNum - 64;
		if (rhythmTone == 0 || rhythmTone == 1) {
			noteOff(0);
			key = 1;
		} else if (rhythmTone == 2) {
			noteOff(0);
			key = 0;
		}
	} else if (drumTimbreNum == 64 + 6) {
		// CONFIRMED: Two special cases described by Mok (MT-32 family: R07 closed hi-hat, R08 open hi-hat)
		noteOff(0);
		key = 1;
	} else if (drumTimbreNum == 64 + 7) {
		// This noteOff(0) is not performed on MT-32, only LAPC-I
		noteOff(0);
		key = 0;
	}
	if (isOutputMuted(rhythmTemp[drumNum].reverbSwitch)) {
		return;
	}
	int absTimbreNum = drumTimbreNum + 128;
	TimbreParam *timbre = &synth->mt32ram.timbres[absTimbreNum].timbre;
	memcpy(currentInstr, timbre->common.name, 10);
	if (drumCache[drumNum][0].dirty) {
		cacheTimbre(drumCache[drumNum], timbre);
	}
#if MT32EMU_MONITOR_INSTRUMENTS > 0
	synth->printDebug("%s (%s): Start poly (drum %d, timbre %d): midiKey %u, key %u, velo %u, mod %u, exp %u, bend %u", name, currentInstr, drumNum, absTimbreNum, midiKey, key, velocity, modulation, expression, pitchBend);
#if MT32EMU_MONITOR_INSTRUMENTS > 1
	// According to info from Mok, keyShift does not appear to affect anything on rhythm part on LAPC-I, but may do on MT-32 - needs investigation
	synth->printDebug(" Patch: (timbreGroup %u), (timbreNum %u), (keyShift %u), fineTune %u, benderRange %u, assignMode %u, (reverbSwitch %u)", patchTemp->patch.timbreGroup, patchTemp->patch.timbreNum, patchTemp->patch.keyShift, patchTemp->patch.fineTune, patchTemp->patch.benderRange, patchTemp->patch.assignMode, patchTemp->patch.reverbSwitch);
	synth->printDebug(" PatchTemp: outputLevel %u, (panpot %u)", patchTemp->outputLevel, patchTemp->panpot);
	synth->printDebug(" RhythmTemp: timbre %u, outputLevel %u, panpot %u, reverbSwitch %u", rhythmTemp[drumNum].timbre, rhythmTemp[drumNum].outputLevel, rhythmTemp[drumNum].panpot, rhythmTemp[drumNum].reverbSwitch);
#endif
#endif
	playPoly(drumCache[drumNum], &rhythmTemp[drumNum], midiKey, key, velocity);
}

void Part::noteOn(unsigned int midiKey, unsigned int velocity) {
	if (synth->controlROMFeatures->d110MemoryMap) {
		// D-110 key range (timbre temporary area bytes 0A/0B, the MT-32's dummy bytes).
		if (midiKey < patchTemp->dummyv[0] || midiKey > patchTemp->dummyv[1]) {
			return;
		}
		if (isOutputMuted(patchTemp->patch.reverbSwitch)) {
			return;
		}
	}
	unsigned int key = midiKeyToKey(midiKey);
	if (patchCache[0].dirty) {
		cacheTimbre(patchCache, timbreTemp);
	}
#if MT32EMU_MONITOR_INSTRUMENTS > 0
	synth->printDebug("%s (%s): Start poly: midiKey %u, key %u, velo %u, mod %u, exp %u, bend %u", name, currentInstr, midiKey, key, velocity, modulation, expression, pitchBend);
#if MT32EMU_MONITOR_INSTRUMENTS > 1
	synth->printDebug(" Patch: timbreGroup %u, timbreNum %u, keyShift %u, fineTune %u, benderRange %u, assignMode %u, reverbSwitch %u", patchTemp->patch.timbreGroup, patchTemp->patch.timbreNum, patchTemp->patch.keyShift, patchTemp->patch.fineTune, patchTemp->patch.benderRange, patchTemp->patch.assignMode, patchTemp->patch.reverbSwitch);
	synth->printDebug(" PatchTemp: outputLevel %u, panpot %u", patchTemp->outputLevel, patchTemp->panpot);
#endif
#endif
	if (synth->isMIDIExtensionsEnabled()) {
		// Portamento: the new note glides from CC 84's key, or with portamento on from the last note's. When a note
		// still sounds on CC 84's key, that note moves to the new key instead (legato).
		if (portamentoControlKey >= 0) {
			const unsigned int fromKey = midiKeyToKey(unsigned(portamentoControlKey));
			if (legatoNote(fromKey, key)) {
				portamentoControlKey = -1;
				lastNoteKey = int(midiKey);
				return;
			}
			portamentoSourceKey = int(fromKey);
		} else if (portamento && lastNoteKey >= 0) {
			portamentoSourceKey = int(midiKeyToKey(unsigned(lastNoteKey)));
		}
	}
	playPoly(patchCache, NULL, midiKey, key, velocity);
	portamentoSourceKey = -1;
	if (synth->isMIDIExtensionsEnabled() && !synth->isAbortingPoly()) {
		// While an abort holds the note-on back, it is played again once the abort is done.
		portamentoControlKey = -1;
		lastNoteKey = int(midiKey);
	}
}

bool Part::abortFirstPoly(unsigned int key) {
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		if (poly->getKey() == key) {
			return poly->startAbort();
		}
	}
	return false;
}

bool Part::abortFirstPoly(PolyState polyState) {
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		if (poly->getState() == polyState) {
			return poly->startAbort();
		}
	}
	return false;
}

bool Part::abortFirstPolyPreferHeld() {
	if (abortFirstPoly(POLY_Held)) {
		return true;
	}
	return abortFirstPoly();
}

bool Part::abortFirstPoly() {
	if (activePolys.isEmpty()) {
		return false;
	}
	return activePolys.getFirst()->startAbort();
}

void Part::playPoly(const PatchCache cache[4], const MemParams::RhythmTemp *rhythmTemp, unsigned int midiKey, unsigned int key, unsigned int velocity) {
	// CONFIRMED: Even in single-assign mode, we don't abort playing polys if the timbre to play is completely muted.
	unsigned int needPartials = cache[0].partialCount;
	if (needPartials == 0) {
		synth->printDebug("%s (%s): Completely muted instrument", name, currentInstr);
		return;
	}

	if ((patchTemp->patch.assignMode & 2) == 0) {
		// Single-assign mode
		abortFirstPoly(key);
		if (synth->isAbortingPoly()) return;
	}

	if (!synth->partialManager->freePartials(needPartials, partNum)) {
#if MT32EMU_MONITOR_PARTIALS > 0
		synth->printDebug("%s (%s): Insufficient free partials to play key %d (velocity %d); needed=%d, free=%d, assignMode=%d", name, currentInstr, midiKey, velocity, needPartials, synth->partialManager->getFreePartialCount(), patchTemp->patch.assignMode);
		synth->printPartialUsage();
#endif
		return;
	}
	if (synth->isAbortingPoly()) return;

	Poly *poly = synth->partialManager->assignPolyToPart(this);
	if (poly == NULL) {
		synth->printDebug("%s (%s): No free poly to play key %d (velocity %d)", name, currentInstr, midiKey, velocity);
		return;
	}
	if (patchTemp->patch.assignMode & 1) {
		// Priority to data first received
		activePolys.prepend(poly);
	} else {
		activePolys.append(poly);
	}

	Partial *partials[4];
	for (int x = 0; x < 4; x++) {
		if (cache[x].playPartial) {
			partials[x] = synth->partialManager->allocPartial(partNum);
			activePartialCount++;
		} else {
			partials[x] = NULL;
		}
	}
	poly->reset(key, velocity, cache[0].sustain, partials);

	for (int x = 0; x < 4; x++) {
		if (partials[x] != NULL) {
#if MT32EMU_MONITOR_PARTIALS > 2
			synth->printDebug("%s (%s): Allocated partial %d", name, currentInstr, partials[x]->debugGetPartialNum());
#endif
			partials[x]->startPartial(this, poly, &cache[x], rhythmTemp, partials[cache[x].structurePair]);
		}
	}
#if MT32EMU_MONITOR_PARTIALS > 1
	synth->printPartialUsage();
#endif
	synth->reportHandler->onPolyStateChanged(Bit8u(partNum));
}

void Part::allNotesOff() {
	// The MIDI specification states - and Mok confirms - that all notes off (0x7B)
	// should treat the hold pedal as usual.
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		// FIXME: This has special handling of key 0 in NoteOff that Mok has not yet confirmed applies to AllNotesOff.
		// if (poly->canSustain() || poly->getKey() == 0) {
		// FIXME: The real devices are found to be ignoring non-sustaining polys while processing AllNotesOff. Need to be confirmed.
		if (poly->canSustain()) {
			poly->noteOff(holdpedal);
		}
	}
}

void Part::allSoundOff() {
	// MIDI "All sound off" (0x78) should release notes immediately regardless of the hold pedal.
	// This controller is not actually implemented by the synths, though (according to the docs and Mok) -
	// we're only using this method internally.
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		poly->startDecay();
	}
}

void Part::stopPedalHold() {
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		poly->stopPedalHold();
	}
}

void RhythmPart::noteOff(unsigned int midiKey) {
	stopNote(midiKey);
}

void Part::noteOff(unsigned int midiKey) {
	stopNote(midiKeyToKey(midiKey));
}

void Part::stopNote(unsigned int key) {
#if MT32EMU_MONITOR_INSTRUMENTS > 0
	synth->printDebug("%s (%s): stopping key %d", name, currentInstr, key);
#endif

	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		// Generally, non-sustaining instruments ignore note off. They die away eventually anyway.
		// Key 0 (only used by special cases on rhythm part) reacts to note off even if non-sustaining or pedal held.
		if (poly->getKey() == key && (poly->canSustain() || key == 0)) {
			if (poly->noteOff(holdpedal && key != 0)) {
				break;
			}
		}
	}
}

const MemParams::PatchTemp *Part::getPatchTemp() const {
	return patchTemp;
}

unsigned int Part::getActivePartialCount() const {
	return activePartialCount;
}

const Poly *Part::getFirstActivePoly() const {
	return activePolys.getFirst();
}

unsigned int Part::getActiveNonReleasingPartialCount() const {
	unsigned int activeNonReleasingPartialCount = 0;
	for (Poly *poly = activePolys.getFirst(); poly != NULL; poly = poly->getNext()) {
		if (poly->getState() != POLY_Releasing) {
			activeNonReleasingPartialCount += poly->getActivePartialCount();
		}
	}
	return activeNonReleasingPartialCount;
}

Synth *Part::getSynth() const {
	return synth;
}

void Part::partialDeactivated(Poly *poly) {
	activePartialCount--;
	if (!poly->isActive()) {
		activePolys.remove(poly);
		synth->partialManager->polyFreed(poly);
		synth->reportHandler->onPolyStateChanged(Bit8u(partNum));
	}
}

PolyList::PolyList() : firstPoly(NULL), lastPoly(NULL) {}

bool PolyList::isEmpty() const {
#ifdef MT32EMU_POLY_LIST_DEBUG
	if ((firstPoly == NULL || lastPoly == NULL) && firstPoly != lastPoly) {
		printf("PolyList: desynchronised firstPoly & lastPoly pointers\n");
	}
#endif
	return firstPoly == NULL && lastPoly == NULL;
}

Poly *PolyList::getFirst() const {
	return firstPoly;
}

Poly *PolyList::getLast() const {
	return lastPoly;
}

void PolyList::prepend(Poly *poly) {
#ifdef MT32EMU_POLY_LIST_DEBUG
	if (poly->getNext() != NULL) {
		printf("PolyList: Non-NULL next field in a Poly being prepended is ignored\n");
	}
#endif
	poly->setNext(firstPoly);
	firstPoly = poly;
	if (lastPoly == NULL) {
		lastPoly = poly;
	}
}

void PolyList::append(Poly *poly) {
#ifdef MT32EMU_POLY_LIST_DEBUG
	if (poly->getNext() != NULL) {
		printf("PolyList: Non-NULL next field in a Poly being appended is ignored\n");
	}
#endif
	poly->setNext(NULL);
	if (lastPoly != NULL) {
#ifdef MT32EMU_POLY_LIST_DEBUG
		if (lastPoly->getNext() != NULL) {
			printf("PolyList: Non-NULL next field in the lastPoly\n");
		}
#endif
		lastPoly->setNext(poly);
	}
	lastPoly = poly;
	if (firstPoly == NULL) {
		firstPoly = poly;
	}
}

Poly *PolyList::takeFirst() {
	Poly *oldFirst = firstPoly;
	firstPoly = oldFirst->getNext();
	if (firstPoly == NULL) {
#ifdef MT32EMU_POLY_LIST_DEBUG
		if (lastPoly != oldFirst) {
			printf("PolyList: firstPoly != lastPoly in a list with a single Poly\n");
		}
#endif
		lastPoly = NULL;
	}
	oldFirst->setNext(NULL);
	return oldFirst;
}

void PolyList::remove(Poly * const polyToRemove) {
	if (polyToRemove == firstPoly) {
		takeFirst();
		return;
	}
	for (Poly *poly = firstPoly; poly != NULL; poly = poly->getNext()) {
		if (poly->getNext() == polyToRemove) {
			if (polyToRemove == lastPoly) {
#ifdef MT32EMU_POLY_LIST_DEBUG
				if (lastPoly->getNext() != NULL) {
					printf("PolyList: Non-NULL next field in the lastPoly\n");
				}
#endif
				lastPoly = poly;
			}
			poly->setNext(polyToRemove->getNext());
			polyToRemove->setNext(NULL);
			break;
		}
	}
}

} // namespace MT32Emu
