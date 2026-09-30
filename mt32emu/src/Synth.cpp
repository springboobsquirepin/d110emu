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

#include <cstddef>
#include <cstdio>

#include "internals.h"

#include "Synth.h"
#include "Analog.h"
#include "BReverbModel.h"
#include "DSeriesReverb.h"
#include "File.h"
#include "MemoryRegion.h"
#include "MidiEventQueue.h"
#include "Part.h"
#include "Partial.h"
#include "PartialManager.h"
#include "Poly.h"
#include "ROMInfo.h"
#include "TVA.h"

#if MT32EMU_MONITOR_SYSEX > 0
#include "mmath.h"
#endif

namespace MT32Emu {

// MIDI interface data transfer rate in samples. Used to simulate the transfer delay.
static const double MIDI_DATA_TRANSFER_RATE = double(SAMPLE_RATE) / 31250.0 * 8.0;

// FIXME: there should be more specific feature sets for various MT-32 control ROM versions
// The D-110 structures mirror the SysEx address map byte for byte.
static_assert(sizeof(D110PatchPartParam) == 12, "D-110 patch part parameters are 12 bytes");
static_assert(sizeof(D110PatchParam) == 128, "D-110 patch memory entries are 128 bytes (01 00 in 7-bit addressing)");
static_assert(sizeof(MemParams::System) == SYSTEM_SIZE_D110, "System area holds the D-110 layout");
static_assert(sizeof(D20PatchParam) == D20_PATCH_SIZE, "D-20 performance patches are 38 bytes");

// A D-20 performance patch as a factory reset (or a lost memory) leaves it, byte for byte as the user's D-20 dumps it:
// "<Initial Patch>", split at C4, the tones in order (A11-A88: upper a01-a64 over lower b01-b64; B11-B88: upper b01-b64
// over lower a01-a64), no key shift or fine tune, bender range 12, POLY 1, reverb on (Medium Hall, Time 6, Level 3),
// balance 50, level 100, and 4 in the dummy byte.
static void initD20Patch(D20PatchParam &patch, unsigned int patchNum) {
	memset(&patch, 0, sizeof(patch));
	const bool bankB = patchNum >= 64;
	patch.keyMode = 2; // Split
	patch.splitPoint = 24; // C4
	patch.upperToneGroup = bankB ? 1 : 0;
	patch.lowerToneGroup = bankB ? 0 : 1;
	patch.upperToneNumber = patch.lowerToneNumber = Bit8u(patchNum & 63);
	patch.lowerKeyShift = patch.upperKeyShift = 24;
	patch.lowerFineTune = patch.upperFineTune = 50;
	patch.lowerBenderRange = patch.upperBenderRange = 12;
	patch.lowerReverbSwitch = patch.upperReverbSwitch = 1;
	patch.reverbMode = 2; // Medium Hall
	patch.reverbTime = 5; // Time 6
	patch.reverbLevel = 3;
	patch.balance = 50;
	patch.patchLevel = 100;
	memcpy(patch.name, "<Initial Patch> ", sizeof(patch.name));
	patch.dummy = 4;
}

static const ControlROMFeatureSet OLD_MT32_COMPATIBLE = {
	true, // quirkBasePitchOverflow
	true, // quirkPitchEnvelopeOverflow
	true, // quirkRingModulationNoMix
	true, // quirkTVAZeroEnvLevels
	true, // quirkPanMult
	true, // quirkKeyShift
	true, // quirkTVFBaseCutoffLimit
	true, // defaultReverbMT32Compatible
	true, // oldMT32AnalogLPF
	false, // defaultChannelsStartAt1
	false, // panLeftToRight
	false // d110MemoryMap
};
static const ControlROMFeatureSet CM32L_COMPATIBLE = {
	false, // quirkBasePitchOverflow
	false, // quirkPitchEnvelopeOverflow
	false, // quirkRingModulationNoMix
	false, // quirkTVAZeroEnvLevels
	false, // quirkPanMult
	false, // quirkKeyShift
	false, // quirkTVFBaseCutoffLimit
	false, // defaultReverbMT32Compatible
	false, // oldMT32AnalogLPF
	false, // defaultChannelsStartAt1
	false, // panLeftToRight
	false // d110MemoryMap
};
static const ControlROMFeatureSet D110_COMPATIBLE = {
	false, // quirkBasePitchOverflow
	false, // quirkPitchEnvelopeOverflow
	false, // quirkRingModulationNoMix
	false, // quirkTVAZeroEnvLevels
	false, // quirkPanMult
	false, // quirkKeyShift
	false, // quirkTVFBaseCutoffLimit
	false, // defaultReverbMT32Compatible
	false, // oldMT32AnalogLPF
	true, // defaultChannelsStartAt1
	true, // panLeftToRight
	true // d110MemoryMap
};

static const ControlROMMap ControlROMMaps[10] = {
	//     ID                Features        PCMmap  PCMc  tmbrA  tmbrAO, tmbrAC tmbrB   tmbrBO  tmbrBC tmbrR   trC rhythm rhyC  rsrv   panpot   prog   rhyMax  patMax  sysMax  timMax  sndGrp sGC
	{ "ctrl_mt32_1_04", OLD_MT32_COMPATIBLE, 0x3000, 128, 0x8000, 0x0000, false, 0xC000, 0x4000, false, 0x3200, 30, 0x73A6, 85, 0x57C7, 0x57E2, 0x57D0, 0x5252, 0x525E, 0x526E, 0x520A, 0x7064, 19 },
	{ "ctrl_mt32_1_05", OLD_MT32_COMPATIBLE, 0x3000, 128, 0x8000, 0x0000, false, 0xC000, 0x4000, false, 0x3200, 30, 0x7414, 85, 0x57C7, 0x57E2, 0x57D0, 0x5252, 0x525E, 0x526E, 0x520A, 0x70CA, 19 },
	{ "ctrl_mt32_1_06", OLD_MT32_COMPATIBLE, 0x3000, 128, 0x8000, 0x0000, false, 0xC000, 0x4000, false, 0x3200, 30, 0x7414, 85, 0x57D9, 0x57F4, 0x57E2, 0x5264, 0x5270, 0x5280, 0x521C, 0x70CA, 19 },
	{ "ctrl_mt32_1_07", OLD_MT32_COMPATIBLE, 0x3000, 128, 0x8000, 0x0000, false, 0xC000, 0x4000, false, 0x3200, 30, 0x73fe, 85, 0x57B1, 0x57CC, 0x57BA, 0x523C, 0x5248, 0x5258, 0x51F4, 0x70B0, 19 }, // MT-32 revision 1
	{"ctrl_mt32_bluer", OLD_MT32_COMPATIBLE, 0x3000, 128, 0x8000, 0x0000, false, 0xC000, 0x4000, false, 0x3200, 30, 0x741C, 85, 0x57E5, 0x5800, 0x57EE, 0x5270, 0x527C, 0x528C, 0x5228, 0x70CE, 19 }, // MT-32 Blue Ridge mod
	{"ctrl_mt32_2_04",   CM32L_COMPATIBLE,   0x8100, 128, 0x8000, 0x8000, true,  0x8080, 0x8000, true,  0x8500, 30, 0x8580, 85, 0x4F5D, 0x4F78, 0x4F66, 0x4899, 0x489D, 0x48B6, 0x48CD, 0x5A58, 19 },
	{"ctrl_cm32l_1_00",  CM32L_COMPATIBLE,   0x8100, 256, 0x8000, 0x8000, true,  0x8080, 0x8000, true,  0x8500, 64, 0x8580, 85, 0x4F65, 0x4F80, 0x4F6E, 0x48A1, 0x48A5, 0x48BE, 0x48D5, 0x5A6C, 19 },
	{"ctrl_cm32l_1_02",  CM32L_COMPATIBLE,   0x8100, 256, 0x8000, 0x8000, true,  0x8080, 0x8000, true,  0x8500, 64, 0x8580, 85, 0x4F93, 0x4FAE, 0x4F9C, 0x48CB, 0x48CF, 0x48E8, 0x48FF, 0x5A96, 19 }, // CM-32L
	{"ctrl_d110_1_10_1",  D110_COMPATIBLE,    0x8900, 256, 0x8000, 0x8000, true,  0x8080, 0x8000, true,  0x8D00, 64, 0x8D80, 85, 0x2CFF, 0x2D11, 0x2D08, 0x4A45, 0x4A49, 0x4A62, 0x4A79, 0x0000, 0  },
	{"ctrl_d110_1_10_2",  D110_COMPATIBLE,    0x8900, 256, 0x8000, 0x8000, true,  0x8080, 0x8000, true,  0x8D00, 64, 0x8D80, 85, 0x2CFF, 0x2D11, 0x2D08, 0x4A45, 0x4A49, 0x4A62, 0x4A79, 0x0000, 0  }
	// (Note that old MT-32 ROMs actually have 86 entries for rhythmTemp)
};

static const PartialState PARTIAL_PHASE_TO_STATE[8] = {
	PartialState_ATTACK, PartialState_ATTACK, PartialState_ATTACK, PartialState_ATTACK,
	PartialState_SUSTAIN, PartialState_SUSTAIN, PartialState_RELEASE, PartialState_INACTIVE
};

static inline PartialState getPartialState(PartialManager *partialManager, unsigned int partialNum) {
	const Partial *partial = partialManager->getPartial(partialNum);
	return partial->isActive() ? PARTIAL_PHASE_TO_STATE[partial->getTVA()->getPhase()] : PartialState_INACTIVE;
}

template <class I, class O>
static inline void convertSampleFormat(const I *inBuffer, O *outBuffer, const Bit32u len) {
	if (inBuffer == NULL || outBuffer == NULL) return;

	const I *inBufferEnd = inBuffer + len;
	while (inBuffer < inBufferEnd) {
		*(outBuffer++) = Synth::convertSample(*(inBuffer++));
	}
}

class Renderer {
protected:
	Synth &synth;

	void printDebug(const char *msg) const {
		synth.printDebug("%s", msg);
	}

	bool isActivated() const {
		return synth.activated;
	}

	bool isAbortingPoly() const {
		return synth.isAbortingPoly();
	}

	Analog &getAnalog() const {
		return *synth.analog;
	}

	Analog *getPartAnalog(unsigned int stream) const {
		return synth.partAnalogs[stream];
	}

	MidiEventQueue &getMidiQueue() {
		return *synth.midiQueue;
	}

	PartialManager &getPartialManager() {
		return *synth.partialManager;
	}

	BReverbModel &getReverbModel() {
		return *synth.reverbModel;
	}

	Bit32u getRenderedSampleCount() {
		return synth.renderedSampleCount;
	}

	void incRenderedSampleCount(const Bit32u count) {
		synth.renderedSampleCount += count;
	}

public:
	Renderer(Synth &useSynth) : synth(useSynth) {}

	virtual ~Renderer() {}

	virtual void render(IntSample *stereoStream, Bit32u len) = 0;
	virtual void render(FloatSample *stereoStream, Bit32u len) = 0;
	virtual void render(float *stereoStream, float *const *partStreams, float *const *multiStreams, Bit32u len) = 0;
	virtual void renderStreams(const DACOutputStreams<IntSample> &streams, Bit32u len) = 0;
	virtual void renderStreams(const DACOutputStreams<FloatSample> &streams, Bit32u len) = 0;
};

template <class Sample>
class RendererImpl : public Renderer {
	// These buffers are used for building the output streams as they are found at the DAC entrance.
	// The output is mixed down to stereo interleaved further in the analog circuitry emulation.
	Sample tmpNonReverbLeft[MAX_SAMPLES_PER_RUN], tmpNonReverbRight[MAX_SAMPLES_PER_RUN];
	Sample tmpReverbDryLeft[MAX_SAMPLES_PER_RUN], tmpReverbDryRight[MAX_SAMPLES_PER_RUN];
	Sample tmpReverbWetLeft[MAX_SAMPLES_PER_RUN], tmpReverbWetRight[MAX_SAMPLES_PER_RUN];
	// Where the partials of muted parts go (Synth::setMutedParts), not heard.
	Sample tmpMutedLeft[MAX_SAMPLES_PER_RUN], tmpMutedRight[MAX_SAMPLES_PER_RUN];
	// Part outputs (Synth::setPartOutputsAvailable; NULL where not built: all without, the parts' without part streams):
	// each output stream at the DAC entrance (the parts',
	// then the MULTI pairs'), where the partials playing out of it go while a pass renders them (partStreamsWanted), and
	// its output of a pass. Their analogs take silence for the reverb streams they do not have.
	Sample *tmpPartLeft[OUTPUT_STREAM_COUNT], *tmpPartRight[OUTPUT_STREAM_COUNT];
	Sample *partPassBuffers[OUTPUT_STREAM_COUNT];
	Sample tmpSilence[MAX_SAMPLES_PER_RUN];
	// A partial on its part's own output that also feeds the reverb, or on a MULTI output: rendered here, then added to
	// the part's stream and to the reverb's send (which joins the mix's reverb partials at the reverb's input only), or
	// both sides to the MULTI output's channel.
	Sample *tmpScratchLeft, *tmpScratchRight;
	Sample *tmpReverbSendLeft, *tmpReverbSendRight;
	Sample *tmpReverbInputLeft, *tmpReverbInputRight;
	bool partStreamsWanted;
	Bit32u partStreamPosition; // Where produceStreams() writes in the part streams, within doRenderStreams()

	const DACOutputStreams<Sample> tmpBuffers;
	DACOutputStreams<Sample> createTmpBuffers() {
		DACOutputStreams<Sample> buffers = {
			tmpNonReverbLeft, tmpNonReverbRight,
			tmpReverbDryLeft, tmpReverbDryRight,
			tmpReverbWetLeft, tmpReverbWetRight
		};
		return buffers;
	}

public:
	RendererImpl(Synth &useSynth) :
		Renderer(useSynth),
		tmpBuffers(createTmpBuffers()),
		partStreamsWanted(false),
		partStreamPosition(0)
	{
		const bool partOutputs = useSynth.arePartOutputsAvailable();
		for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
			// The streams open() built an analog stage for: all, or only the MULTI pairs'.
			const bool built = getPartAnalog(stream) != NULL;
			tmpPartLeft[stream] = built ? new Sample[MAX_SAMPLES_PER_RUN] : NULL;
			tmpPartRight[stream] = built ? new Sample[MAX_SAMPLES_PER_RUN] : NULL;
			partPassBuffers[stream] = built ? new Sample[MAX_SAMPLES_PER_RUN << 1] : NULL;
		}
		Sample **const sendBuffers[] = {&tmpScratchLeft, &tmpScratchRight, &tmpReverbSendLeft, &tmpReverbSendRight, &tmpReverbInputLeft, &tmpReverbInputRight};
		for (Sample **buffer : sendBuffers) {
			*buffer = partOutputs ? new Sample[MAX_SAMPLES_PER_RUN] : NULL;
		}
		Synth::muteSampleBuffer(tmpSilence, MAX_SAMPLES_PER_RUN);
	}

	~RendererImpl() {
		for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
			delete[] tmpPartLeft[stream];
			delete[] tmpPartRight[stream];
			delete[] partPassBuffers[stream];
		}
		Sample *const sendBuffers[] = {tmpScratchLeft, tmpScratchRight, tmpReverbSendLeft, tmpReverbSendRight, tmpReverbInputLeft, tmpReverbInputRight};
		for (Sample *buffer : sendBuffers) {
			delete[] buffer;
		}
	}

	void render(IntSample *stereoStream, Bit32u len);
	void render(FloatSample *stereoStream, Bit32u len);
	void render(float *stereoStream, float *const *partStreams, float *const *multiStreams, Bit32u len);
	void renderPass(Sample *stereoStream, Bit32u len);
	void renderStreams(const DACOutputStreams<IntSample> &streams, Bit32u len);
	void renderStreams(const DACOutputStreams<FloatSample> &streams, Bit32u len);

	template <class O>
	void doRenderAndConvert(O *stereoStream, Bit32u len);
	void doRender(Sample *stereoStream, Bit32u len);

	template <class O>
	void doRenderAndConvertStreams(const DACOutputStreams<O> &streams, Bit32u len);
	void doRenderStreams(const DACOutputStreams<Sample> &streams, Bit32u len);
	void produceLA32Output(Sample *buffer, Bit32u len);
	void convertSamplesToOutput(Sample *buffer, Bit32u len);
	void produceStreams(const DACOutputStreams<Sample> &streams, Bit32u len);
};

class Extensions {
public:
	RendererType selectedRendererType;
	Bit32s masterTunePitchDelta;
	Bit32s gsMasterTune; // MIDI extensions: the GS master tune, tenths of a cent
	Bit32s gsMasterTunePitchDelta;
	bool niceAmpRamp;
	bool nicePanning;
	bool nicePartialMixing;
	Bit32u mutedParts; // Bit n: part number n is not heard
	bool partOutputsAvailable; // Requested; open() builds the part analogs and the renderer's part streams
	bool partStreamsAvailable; // The parts' streams among them (else only the MULTI pairs')
	Bit32u partOutputMask; // Bit n: part number n's own output is in use
	bool multiOutputs; // The MULTI outputs are outputs of their own
	bool multiPairsStereo; // With multiOutputs: MULTI 1+2, 3+4 and 5+6 as stereo outputs

	// Here we keep the reverse mapping of assigned parts per MIDI channel.
	// NOTE: 0xFF (any value of MAX_PART_COUNT or more) ends the list.
	Bit8u chantable[16][MAX_PART_COUNT];

	// This stores the index of Part in chantable that failed to play and required partial abortion.
	Bit32u abortingPartIx;

	bool preallocatedReverbMemory;

	Bit32u midiEventQueueSize;
	Bit32u midiEventQueueSysexStorageBufferSize;

	bool midiExtensions;
	bool masterVolumeEnabled; // D-110 mode: apply the system area's master volume (MT-32 translation)
	bool sixteenPartMode; // Requested; open() creates parts 9-15 when the control ROM is a D-110
	bool performanceMode;
	bool cardInserted;
	bool altTimbres; // D-110: the extra tone banks d and e are loaded (setAltTimbresEnabled)
	Bit8u performanceChannel; // 0-15
	Bit8u currentPerformance; // 0-127

	// D-110
	Bit8u controlChannel; // 0-15, 16 = off
	Bit8u deviceID; // Unit number less one
	Bit8u currentPatch;
	char d110Display[33]; // Text written to the display area, 16x2 characters
};

Bit32u Synth::getLibraryVersionInt() {
	return (MT32EMU_VERSION_MAJOR << 16) | (MT32EMU_VERSION_MINOR << 8) | (MT32EMU_VERSION_PATCH);
}

const char *Synth::getLibraryVersionString() {
	return MT32EMU_VERSION;
}

Bit8u Synth::calcSysexChecksum(const Bit8u *data, const Bit32u len, const Bit8u initChecksum) {
	unsigned int checksum = -initChecksum;
	for (unsigned int i = 0; i < len; i++) {
		checksum -= data[i];
	}
	return Bit8u(checksum & 0x7f);
}

Bit32u Synth::getStereoOutputSampleRate(AnalogOutputMode analogOutputMode) {
	static const unsigned int SAMPLE_RATES[] = {SAMPLE_RATE, SAMPLE_RATE, SAMPLE_RATE * 3 / 2, SAMPLE_RATE * 3};

	return SAMPLE_RATES[analogOutputMode];
}

Synth::Synth(ReportHandler *useReportHandler) :
	mt32ram(*new MemParams),
	mt32default(*new MemParams),
	extensions(*new Extensions)
{
	opened = false;
	reverbOverridden = false;
	partialCount = DEFAULT_MAX_PARTIALS;
	controlROMMap = NULL;
	controlROMFeatures = NULL;
	controlROMData = NULL;
	controlROMSize = 0;
	memset(soundGroupIx, 0, sizeof(soundGroupIx));

	if (useReportHandler == NULL) {
		reportHandler = new ReportHandler;
		isDefaultReportHandler = true;
	} else {
		reportHandler = useReportHandler;
		isDefaultReportHandler = false;
	}

	extensions.preallocatedReverbMemory = false;
	for (int i = REVERB_MODE_ROOM; i <= REVERB_MODE_TAP_DELAY; i++) {
		reverbModels[i] = NULL;
	}
	reverbModel = NULL;
	dSeriesReverbModel = NULL;
	dSeriesReverbSettings = new DSeriesReverbSettings(DSeriesReverbSettings::getDefaults());
	dSeriesReverbEnabled = true;
	analog = NULL;
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
		partAnalogs[stream] = NULL;
	}
	renderer = NULL;
	setDACInputMode(DACInputMode_NICE);
	setMIDIDelayMode(MIDIDelayMode_DELAY_SHORT_MESSAGES_ONLY);
	setOutputGain(1.0f);
	setReverbOutputGain(1.0f);
	setReversedStereoEnabled(false);
	setNiceAmpRampEnabled(true);
	setNicePanningEnabled(false);
	setNicePartialMixingEnabled(false);
	selectRendererType(RendererType_BIT16S);

	patchTempMemoryRegion = NULL;
	rhythmTempMemoryRegion = NULL;
	timbreTempMemoryRegion = NULL;
	patchesMemoryRegion = NULL;
	timbresMemoryRegion = NULL;
	systemMemoryRegion = NULL;
	displayMemoryRegion = NULL;
	resetMemoryRegion = NULL;
	d110PatchesMemoryRegion = NULL;
	d20PatchesMemoryRegion = NULL;
	d20RhythmSetupMemoryRegion = NULL;
	d20SystemExtMemoryRegion = NULL;
	writeRequestMemoryRegion = NULL;
	extPatchTempMemoryRegion = NULL;
	extTimbreTempMemoryRegion = NULL;
	extSystemMemoryRegion = NULL;
	paddedTimbreMaxTable = NULL;
	extensions.midiExtensions = false;
	extensions.gsMasterTune = 0;
	extensions.gsMasterTunePitchDelta = 0;
	extensions.masterVolumeEnabled = false;
	extensions.sixteenPartMode = false;
	extensions.performanceMode = false;
	extensions.cardInserted = false;
	extensions.altTimbres = false;
	extensions.mutedParts = 0;
	extensions.partOutputsAvailable = false;
	extensions.partStreamsAvailable = false;
	extensions.partOutputMask = 0;
	extensions.multiOutputs = false;
	extensions.multiPairsStereo = false;
	cardTonesMemoryRegion = NULL;
	cardTimbresMemoryRegion = NULL;
	cardPatchesMemoryRegion = NULL;
	partFinePanMemoryRegion = NULL;
	rhythmFinePanMemoryRegion = NULL;
	d20PatternsMemoryRegion = NULL;
	d20PresetPatternsMemoryRegion = NULL;
	d20RhythmTrackMemoryRegion = NULL;
	extensions.performanceChannel = 0;
	extensions.currentPerformance = 0;
	d20PatchTempMemoryRegion = NULL;
	partCount = BASE_PART_COUNT;
	extensions.controlChannel = 16;
	extensions.deviceID = 0x10;
	extensions.currentPatch = 0;
	memset(extensions.d110Display, ' ', 32);
	extensions.d110Display[32] = 0;

	partialManager = NULL;
	pcmWaves = NULL;
	pcmROMData = NULL;
	soundGroupNames = NULL;
	midiQueue = NULL;
	extensions.midiEventQueueSize = DEFAULT_MIDI_EVENT_QUEUE_SIZE;
	extensions.midiEventQueueSysexStorageBufferSize = 0;
	lastReceivedMIDIEventTimestamp = 0;
	memset(parts, 0, sizeof(parts));
	renderedSampleCount = 0;
}

Synth::~Synth() {
	close(); // Make sure we're closed and everything is freed
	delete dSeriesReverbSettings;
	if (isDefaultReportHandler) {
		delete reportHandler;
	}
	delete &mt32ram;
	delete &mt32default;
	delete &extensions;
}

void ReportHandler::showLCDMessage(const char *data) {
	printf("WRITE-LCD: %s\n", data);
}

void ReportHandler::printDebug(const char *fmt, va_list list) {
	vprintf(fmt, list);
	printf("\n");
}

void Synth::newTimbreSet(Bit8u partNum, Bit8u timbreGroup, Bit8u timbreNumber, const char patchName[]) {
	if (controlROMMap->soundGroupsCount == 0) {
		// Control ROMs without a sound group table (D-110) have no group names to report.
		reportHandler->onProgramChanged(partNum, NULL, patchName);
		return;
	}
	const char *soundGroupName;
	switch (timbreGroup) {
	case 1:
		timbreNumber += 64;
		// Fall-through
	case 0:
		soundGroupName = soundGroupNames[soundGroupIx[timbreNumber]];
		break;
	case 2:
		soundGroupName = soundGroupNames[controlROMMap->soundGroupsCount - 2];
		break;
	case 3:
		soundGroupName = soundGroupNames[controlROMMap->soundGroupsCount - 1];
		break;
	default:
		soundGroupName = NULL;
		break;
	}
	reportHandler->onProgramChanged(partNum, soundGroupName, patchName);
}

#define MT32EMU_PRINT_DEBUG \
	va_list ap; \
	va_start(ap, fmt); \
	reportHandler->printDebug(fmt, ap); \
	va_end(ap);

#if MT32EMU_DEBUG_SAMPLESTAMPS > 0
static inline void printSamplestamp(ReportHandler *reportHandler, const char *fmt, ...) {
	MT32EMU_PRINT_DEBUG
}
#endif

void Synth::printDebug(const char *fmt, ...) {
#if MT32EMU_DEBUG_SAMPLESTAMPS > 0
	printSamplestamp(reportHandler, "[%u]", renderedSampleCount);
#endif
	MT32EMU_PRINT_DEBUG
}

#undef MT32EMU_PRINT_DEBUG

void Synth::setReverbEnabled(bool newReverbEnabled) {
	if (!opened) return;
	if (isReverbEnabled() == newReverbEnabled) return;
	if (newReverbEnabled) {
		bool oldReverbOverridden = reverbOverridden;
		reverbOverridden = false;
		refreshSystemReverbParameters();
		reverbOverridden = oldReverbOverridden;
	} else {
		if (!extensions.preallocatedReverbMemory) {
			reverbModel->close();
		}
		reverbModel = NULL;
	}
}

bool Synth::isReverbEnabled() const {
	return reverbModel != NULL;
}

void Synth::setReverbOverridden(bool newReverbOverridden) {
	reverbOverridden = newReverbOverridden;
}

bool Synth::isReverbOverridden() const {
	return reverbOverridden;
}

void Synth::setReverbCompatibilityMode(bool mt32CompatibleMode) {
	if (!opened || (isMT32ReverbCompatibilityMode() == mt32CompatibleMode)) return;
	bool oldReverbEnabled = isReverbEnabled();
	setReverbEnabled(false);
	for (int i = REVERB_MODE_ROOM; i <= REVERB_MODE_TAP_DELAY; i++) {
		delete reverbModels[i];
	}
	initReverbModels(mt32CompatibleMode);
	setReverbEnabled(oldReverbEnabled);
	setReverbOutputGain(reverbOutputGain);
}

bool Synth::isMT32ReverbCompatibilityMode() const {
	return opened && (reverbModels[REVERB_MODE_ROOM]->isMT32Compatible(REVERB_MODE_ROOM));
}

bool Synth::isDefaultReverbMT32Compatible() const {
	return opened && controlROMFeatures->defaultReverbMT32Compatible;
}

void Synth::preallocateReverbMemory(bool enabled) {
	if (extensions.preallocatedReverbMemory == enabled) return;
	extensions.preallocatedReverbMemory = enabled;
	if (!opened) return;
	for (int i = REVERB_MODE_ROOM; i <= REVERB_MODE_TAP_DELAY; i++) {
		if (enabled) {
			reverbModels[i]->open();
		} else if (reverbModel != reverbModels[i]) {
			reverbModels[i]->close();
		}
	}
	if (dSeriesReverbModel != NULL) {
		if (enabled) {
			dSeriesReverbModel->open();
		} else if (reverbModel != dSeriesReverbModel) {
			dSeriesReverbModel->close();
		}
	}
}

void Synth::setDSeriesReverbEnabled(bool enabled) {
	if (dSeriesReverbEnabled == enabled) return;
	dSeriesReverbEnabled = enabled;
	if (!opened || !isReverbEnabled()) return;
	// Not a reverb setting from MIDI: it applies while those are overridden too.
	const bool oldReverbOverridden = reverbOverridden;
	reverbOverridden = false;
	refreshSystemReverbParameters();
	reverbOverridden = oldReverbOverridden;
}

bool Synth::isDSeriesReverbEnabled() const {
	return dSeriesReverbEnabled;
}

void Synth::setDSeriesReverbSettings(const DSeriesReverbSettings &settings) {
	*dSeriesReverbSettings = settings;
	dSeriesReverbSettings->clamp();
	if (dSeriesReverbModel != NULL) dSeriesReverbModel->setSettings(*dSeriesReverbSettings);
}

void Synth::setDACInputMode(DACInputMode mode) {
	dacInputMode = mode;
}

DACInputMode Synth::getDACInputMode() const {
	return dacInputMode;
}

void Synth::setMIDIDelayMode(MIDIDelayMode mode) {
	midiDelayMode = mode;
}

MIDIDelayMode Synth::getMIDIDelayMode() const {
	return midiDelayMode;
}

void Synth::setOutputGain(float newOutputGain) {
	if (newOutputGain < 0.0f) newOutputGain = -newOutputGain;
	outputGain = newOutputGain;
	if (analog != NULL) analog->setSynthOutputGain(newOutputGain);
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
		if (partAnalogs[stream] != NULL) partAnalogs[stream]->setSynthOutputGain(newOutputGain);
	}
}

float Synth::getOutputGain() const {
	return outputGain;
}

void Synth::setReverbOutputGain(float newReverbOutputGain) {
	if (newReverbOutputGain < 0.0f) newReverbOutputGain = -newReverbOutputGain;
	reverbOutputGain = newReverbOutputGain;
	if (analog != NULL) analog->setReverbOutputGain(newReverbOutputGain, isMT32ReverbCompatibilityMode());
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
		if (partAnalogs[stream] != NULL) partAnalogs[stream]->setReverbOutputGain(newReverbOutputGain, isMT32ReverbCompatibilityMode());
	}
}

float Synth::getReverbOutputGain() const {
	return reverbOutputGain;
}

void Synth::setReversedStereoEnabled(bool enabled) {
	reversedStereoEnabled = enabled;
}

bool Synth::isReversedStereoEnabled() const {
	return reversedStereoEnabled;
}

void Synth::setNiceAmpRampEnabled(bool enabled) {
	extensions.niceAmpRamp = enabled;
}

bool Synth::isNiceAmpRampEnabled() const {
	return extensions.niceAmpRamp;
}

void Synth::setNicePanningEnabled(bool enabled) {
	extensions.nicePanning = enabled;
}

void Synth::setMutedParts(Bit32u partMask) {
	extensions.mutedParts = partMask;
}

void Synth::resetD20RhythmTrack() {
	// As a D-20 comes from the factory: it plays each preset twice, P-11 to P-48, then stops. Entries past the length
	// are unused.
	memset(mt32ram.d20RhythmTrack, 0, sizeof(mt32ram.d20RhythmTrack));
	mt32ram.d20RhythmTrack[0] = 64; // Length LSB (MSB 0)
	for (Bit8u bar = 0; bar < 64; bar++) mt32ram.d20RhythmTrack[2 + bar] = Bit8u(bar / 2);
}

Bit32u Synth::getMutedParts() const {
	return extensions.mutedParts;
}

void Synth::setPartOutputsAvailable(bool available, bool partStreams) {
	extensions.partOutputsAvailable = available;
	extensions.partStreamsAvailable = available && partStreams;
}

bool Synth::arePartOutputsAvailable() const {
	return extensions.partOutputsAvailable;
}

void Synth::setPartOutputMask(Bit32u partMask) {
	extensions.partOutputMask = partMask;
}

Bit32u Synth::getPartOutputMask() const {
	return extensions.partOutputMask;
}

void Synth::setMultiOutputsEnabled(bool enabled) {
	extensions.multiOutputs = enabled;
}

bool Synth::areMultiOutputsEnabled() const {
	return extensions.multiOutputs;
}

void Synth::setMultiPairsStereo(bool enabled) {
	extensions.multiPairsStereo = enabled;
}

bool Synth::areMultiPairsStereo() const {
	return extensions.multiPairsStereo;
}

bool Synth::isPartOutputRouted(Bit32u partNumber) const {
	return extensions.partStreamsAvailable && partNumber < MAX_PART_COUNT && ((extensions.partOutputMask >> partNumber) & 1) != 0;
}

bool Synth::isNicePanningEnabled() const {
	return extensions.nicePanning;
}

void Synth::setNicePartialMixingEnabled(bool enabled) {
	extensions.nicePartialMixing = enabled;
}

bool Synth::isNicePartialMixingEnabled() const {
	return extensions.nicePartialMixing;
}

bool Synth::loadControlROM(const ROMImage &controlROMImage) {
	File *file = controlROMImage.getFile();
	const ROMInfo *controlROMInfo = controlROMImage.getROMInfo();
	if ((controlROMInfo == NULL)
			|| (controlROMInfo->type != ROMInfo::Control)
			|| (controlROMInfo->pairType != ROMInfo::Full)) {
#if MT32EMU_MONITOR_INIT
		printDebug("Invalid Control ROM Info provided");
#endif
		return false;
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Found Control ROM: %s, %s", controlROMInfo->shortName, controlROMInfo->description);
#endif
	const Bit8u *fileData = file->getData();
	delete[] controlROMData;
	controlROMSize = Bit32u(file->getSize());
	controlROMData = new Bit8u[controlROMSize];
	memcpy(controlROMData, fileData, controlROMSize);

	// Control ROM successfully loaded, now check whether it's a known type
	controlROMMap = NULL;
	controlROMFeatures = NULL;
	for (unsigned int i = 0; i < sizeof(ControlROMMaps) / sizeof(ControlROMMaps[0]); i++) {
		if (strcmp(controlROMInfo->shortName, ControlROMMaps[i].shortName) == 0) {
			controlROMMap = &ControlROMMaps[i];
			controlROMFeatures = &controlROMMap->featureSet;
			return true;
		}
	}
#if MT32EMU_MONITOR_INIT
	printDebug("Control ROM failed to load");
#endif
	return false;
}

bool Synth::loadPCMROM(const ROMImage &pcmROMImage) {
	File *file = pcmROMImage.getFile();
	const ROMInfo *pcmROMInfo = pcmROMImage.getROMInfo();
	if ((pcmROMInfo == NULL)
			|| (pcmROMInfo->type != ROMInfo::PCM)
			|| (pcmROMInfo->pairType != ROMInfo::Full)) {
		return false;
	}
#if MT32EMU_MONITOR_INIT
	printDebug("Found PCM ROM: %s, %s", pcmROMInfo->shortName, pcmROMInfo->description);
#endif
	size_t fileSize = file->getSize();
	if (fileSize != (2 * pcmROMSize)) {
#if MT32EMU_MONITOR_INIT
		printDebug("PCM ROM file has wrong size (expected %d, got %d)", 2 * pcmROMSize, fileSize);
#endif
		return false;
	}
	const Bit8u *fileData = file->getData();
	for (size_t i = 0; i < pcmROMSize; i++) {
		Bit8u s = *(fileData++);
		Bit8u c = *(fileData++);

		int order[16] = {0, 9, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 8};

		Bit16s log = 0;
		for (int u = 0; u < 15; u++) {
			int bit;
			if (order[u] < 8) {
				bit = (s >> (7 - order[u])) & 0x1;
			} else {
				bit = (c >> (7 - (order[u] - 8))) & 0x1;
			}
			log = log | Bit16s(bit << (15 - u));
		}
		pcmROMData[i] = log;
	}
	return true;
}

bool Synth::initPCMList(Bit16u mapAddress, Bit16u count) {
	ControlROMPCMStruct *tps = reinterpret_cast<ControlROMPCMStruct *>(&controlROMData[mapAddress]);
	for (int i = 0; i < count; i++) {
		Bit32u rAddr = tps[i].pos * 0x800;
		Bit32u rLenExp = (tps[i].len & 0x70) >> 4;
		Bit32u rLen = 0x800 << rLenExp;
		if (rAddr + rLen > pcmROMSize) {
			printDebug("Control ROM error: Wave map entry %d points to invalid PCM address 0x%04X, length 0x%04X", i, rAddr, rLen);
			return false;
		}
		pcmWaves[i].addr = rAddr;
		pcmWaves[i].len = rLen;
		pcmWaves[i].loop = (tps[i].len & 0x80) != 0;
		pcmWaves[i].controlROMPCMStruct = &tps[i];
		//int pitch = (tps[i].pitchMSB << 8) | tps[i].pitchLSB;
		//bool unaffectedByMasterTune = (tps[i].len & 0x01) == 0;
		//printDebug("PCM %d: pos=%d, len=%d, pitch=%d, loop=%s, unaffectedByMasterTune=%s", i, rAddr, rLen, pitch, pcmWaves[i].loop ? "YES" : "NO", unaffectedByMasterTune ? "YES" : "NO");
	}
	return false;
}

bool Synth::initCompressedTimbre(Bit16u timbreNum, const Bit8u *src, Bit32u srcLen) {
	// "Compressed" here means that muted partials aren't present in ROM (except in the case of partial 0 being muted).
	// Instead the data from the previous unmuted partial is used.
	if (srcLen < sizeof(TimbreParam::CommonParam)) {
		return false;
	}
	TimbreParam *timbre = &mt32ram.timbres[timbreNum].timbre;
	timbresMemoryRegion->write(timbreNum, 0, src, sizeof(TimbreParam::CommonParam), true);
	unsigned int srcPos = sizeof(TimbreParam::CommonParam);
	unsigned int memPos = sizeof(TimbreParam::CommonParam);
	for (int t = 0; t < 4; t++) {
		if (t != 0 && ((timbre->common.partialMute >> t) & 0x1) == 0x00) {
			// This partial is muted - we'll copy the previously copied partial, then
			srcPos -= sizeof(TimbreParam::PartialParam);
		} else if (srcPos + sizeof(TimbreParam::PartialParam) >= srcLen) {
			return false;
		}
		timbresMemoryRegion->write(timbreNum, memPos, src + srcPos, sizeof(TimbreParam::PartialParam));
		srcPos += sizeof(TimbreParam::PartialParam);
		memPos += sizeof(TimbreParam::PartialParam);
	}
	return true;
}

bool Synth::initTimbres(Bit16u mapAddress, Bit16u offset, Bit16u count, Bit16u startTimbre, bool compressed) {
	const Bit8u *timbreMap = &controlROMData[mapAddress];
	for (Bit16u i = 0; i < count * 2; i += 2) {
		// 32-bit because offset + a 16-bit map entry can exceed 64 KiB in the D-110 control ROM.
		Bit32u address = (timbreMap[i + 1] << 8) | timbreMap[i];
		if (!compressed && (address + offset + sizeof(TimbreParam) > controlROMSize)) {
			printDebug("Control ROM error: Timbre map entry 0x%04x for timbre %d points to invalid timbre address 0x%04x", i, startTimbre, address);
			return false;
		}
		address += offset;
		if (compressed) {
			if (!initCompressedTimbre(startTimbre, &controlROMData[address], controlROMSize - address)) {
				printDebug("Control ROM error: Timbre map entry 0x%04x for timbre %d points to invalid timbre at 0x%04x", i, startTimbre, address);
				return false;
			}
		} else {
			timbresMemoryRegion->write(startTimbre, 0, &controlROMData[address], sizeof(TimbreParam), true);
		}
		startTimbre++;
	}
	return true;
}

void Synth::initReverbModels(bool mt32CompatibleMode) {
	for (int mode = REVERB_MODE_ROOM; mode <= REVERB_MODE_TAP_DELAY; mode++) {
		reverbModels[mode] = BReverbModel::createBReverbModel(ReverbMode(mode), mt32CompatibleMode, getSelectedRendererType());

		if (extensions.preallocatedReverbMemory) {
			reverbModels[mode]->open();
		}
	}
}

void Synth::initSoundGroups(char newSoundGroupNames[][9]) {
	memcpy(soundGroupIx, &controlROMData[controlROMMap->soundGroupsTable - sizeof(soundGroupIx)], sizeof(soundGroupIx));
	const SoundGroup *table = reinterpret_cast<SoundGroup *>(&controlROMData[controlROMMap->soundGroupsTable]);
	for (unsigned int i = 0; i < controlROMMap->soundGroupsCount; i++) {
		memcpy(&newSoundGroupNames[i][0], table[i].name, sizeof(table[i].name));
	}
}

bool Synth::open(const ROMImage &controlROMImage, const ROMImage &pcmROMImage, AnalogOutputMode analogOutputMode) {
	return open(controlROMImage, pcmROMImage, DEFAULT_MAX_PARTIALS, analogOutputMode);
}

bool Synth::open(const ROMImage &controlROMImage, const ROMImage &pcmROMImage, Bit32u usePartialCount, AnalogOutputMode analogOutputMode) {
	if (opened) {
		return false;
	}
	partialCount = usePartialCount;
	abortingPoly = NULL;
	extensions.abortingPartIx = 0;

	// This is to help detect bugs
	memset(&mt32ram, '?', sizeof(mt32ram));

#if MT32EMU_MONITOR_INIT
	printDebug("Loading Control ROM");
#endif
	if (!loadControlROM(controlROMImage)) {
		printDebug("Init Error - Missing or invalid Control ROM image");
		reportHandler->onErrorControlROM();
		dispose();
		return false;
	}
	partCount = extensions.sixteenPartMode && isD110() ? MAX_PART_COUNT : BASE_PART_COUNT;

	initMemoryRegions();

	// 512KB PCM ROM for MT-32, etc.
	// 1MB PCM ROM for CM-32L, LAPC-I, CM-64, CM-500
	// Note that the size below is given in samples (16-bit), not bytes
	pcmROMSize = controlROMMap->pcmCount == 256 ? 512 * 1024 : 256 * 1024;
	pcmROMData = new Bit16s[pcmROMSize];

#if MT32EMU_MONITOR_INIT
	printDebug("Loading PCM ROM");
#endif
	if (!loadPCMROM(pcmROMImage)) {
		printDebug("Init Error - Missing PCM ROM image");
		reportHandler->onErrorPCMROM();
		dispose();
		return false;
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Reverb Models");
#endif
	bool mt32CompatibleReverb = controlROMFeatures->defaultReverbMT32Compatible;
#if MT32EMU_MONITOR_INIT
	printDebug("Using %s Compatible Reverb Models", mt32CompatibleReverb ? "MT-32" : "CM-32L");
#endif
	initReverbModels(mt32CompatibleReverb);
	if (controlROMFeatures->d110MemoryMap) {
		dSeriesReverbModel = DSeriesReverbModel::createDSeriesReverbModel(getSelectedRendererType());
		dSeriesReverbModel->setSettings(*dSeriesReverbSettings);
		if (extensions.preallocatedReverbMemory) dSeriesReverbModel->open();
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Timbre Bank A");
#endif
	if (!initTimbres(controlROMMap->timbreAMap, controlROMMap->timbreAOffset, 0x40, 0, controlROMMap->timbreACompressed)) {
		dispose();
		return false;
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Timbre Bank B");
#endif
	if (!initTimbres(controlROMMap->timbreBMap, controlROMMap->timbreBOffset, 0x40, 64, controlROMMap->timbreBCompressed)) {
		dispose();
		return false;
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Timbre Bank R");
#endif
	if (!initTimbres(controlROMMap->timbreRMap, 0, controlROMMap->timbreRCount, 192, true)) {
		dispose();
		return false;
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Timbre Bank M");
#endif
	// CM-64 seems to initialise all bytes in this bank to 0.
	memset(&mt32ram.timbres[128], 0, sizeof(mt32ram.timbres[128]) * 64);
	// The memory card starts blank until one is inserted, the extra banks d and e until the application fills them.
	memset(&mt32ram.timbres[CARD_TONE_BASE], 0, sizeof(mt32ram.timbres[CARD_TONE_BASE]) * (TIMBRE_COUNT - CARD_TONE_BASE));
	memset(mt32ram.cardTimbres, 0, sizeof(mt32ram.cardTimbres));
	memset(mt32ram.cardPatches, 0, sizeof(mt32ram.cardPatches));

	partialManager = new PartialManager(this, parts);

	pcmWaves = new PCMWaveEntry[controlROMMap->pcmCount];

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising PCM List");
#endif
	initPCMList(controlROMMap->pcmTable, controlROMMap->pcmCount);

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Rhythm Temp");
#endif
	memcpy(mt32ram.rhythmTemp, &controlROMData[controlROMMap->rhythmSettings], controlROMMap->rhythmSettingsCount * 4);

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising Patches");
#endif
	// The D-110 initialises timbres and parts to assign mode POLY 3 (multi-assign), from the firmware's table at
	// 2D1AH (key shift 24, fine tune 50, bender 12, assign 2, output 1). In single-assign mode (the MT-32's POLY 1)
	// every repeated note on a key aborts the previous one, and MIDI processing waits for each abort.
	const Bit8u defaultAssignMode = isD110() ? 2 : 0;
	for (Bit8u i = 0; i < 128; i++) {
		PatchParam *patch = &mt32ram.patches[i];
		patch->timbreGroup = i / 64;
		patch->timbreNum = i % 64;
		patch->keyShift = 24;
		patch->fineTune = 50;
		patch->benderRange = 12;
		patch->assignMode = defaultAssignMode;
		patch->reverbSwitch = 1;
		patch->dummy = 0;
	}

#if MT32EMU_MONITOR_INIT
	printDebug("Initialising System");
#endif
	// The MT-32 manual claims that "Standard pitch" is 442Hz (4AH, confirmed on CM-64). The D-110 and D-10/D-20 come
	// from the factory at 440.0 Hz (40H: a D-20 shows it after a factory reset); their pre-release firmware had 442 Hz.
	mt32ram.system.masterTune = isD110() ? 0x40 : 0x4A;
	mt32ram.system.reverbMode = 0; // Confirmed
	mt32ram.system.reverbTime = 5; // Confirmed
	mt32ram.system.reverbLevel = 3; // Confirmed
	memcpy(mt32ram.system.reserveSettings, &controlROMData[controlROMMap->reserveSettings], 9); // Confirmed
	for (Bit8u i = 0; i < 9; i++) {
		// This is the default: {1, 2, 3, 4, 5, 6, 7, 8, 9}
		// An alternative configuration can be selected by holding "Master Volume"
		// and pressing "PART button 1" on the real MT-32's frontpanel.
		// The channel assignment is then {0, 1, 2, 3, 4, 5, 6, 7, 9}
		// The D-110 uses that alternative assignment as its power-up default.
		if (controlROMFeatures->defaultChannelsStartAt1) {
			mt32ram.system.chanAssign[i] = i < 8 ? i : 9;
		} else {
			mt32ram.system.chanAssign[i] = i + 1;
		}
	}
	mt32ram.system.masterVol = 100; // Confirmed
	memcpy(mt32ram.system.patchName, "Patch     ", sizeof(mt32ram.system.patchName));

	bool oldReverbOverridden = reverbOverridden;
	reverbOverridden = false;
	refreshSystem();
	resetMasterTunePitchDelta();
	reverbOverridden = oldReverbOverridden;

	char(*writableSoundGroupNames)[9] = new char[controlROMMap->soundGroupsCount][9];
	soundGroupNames = writableSoundGroupNames;
	if (controlROMMap->soundGroupsCount > 0) {
		initSoundGroups(writableSoundGroupNames);
	}

	// Parts 9-15 start as copies of parts 1-7's power-on settings; their extra area gives them the same partial
	// reserves and channels 9 and 11-16, so that channel 10 stays with the rhythm part and every channel has a part.
	memset(&mt32ram.systemExt, 0, sizeof(mt32ram.systemExt));
	memset(mt32ram.partFinePan, 0, sizeof(mt32ram.partFinePan)); // Every part and key follows its panpot
	memset(mt32ram.rhythmFinePan, 0, sizeof(mt32ram.rhythmFinePan));
	memset(mt32ram.d20Patterns, 0, sizeof(mt32ram.d20Patterns)); // No rhythm patterns until a D-20 dump arrives
	resetD20RhythmTrack();
	for (unsigned int i = 0; i < EXTRA_PART_COUNT; i++) {
		mt32ram.systemExt.reserveSettings[i] = mt32ram.system.reserveSettings[i];
		mt32ram.systemExt.chanAssign[i] = Bit8u(i == 0 ? 8 : i + 9);
	}
	for (Bit32u i = 0; i < partCount; i++) {
		MemParams::PatchTemp *patchTemp = &mt32ram.patchTemp[i];
		const Bit32u settingsIx = i < BASE_PART_COUNT ? i : i - BASE_PART_COUNT;

		// Note that except for the rhythm part, these patch fields will be set in setProgram() below anyway.
		patchTemp->patch.timbreGroup = 0;
		patchTemp->patch.timbreNum = 0;
		patchTemp->patch.keyShift = 24;
		patchTemp->patch.fineTune = 50;
		patchTemp->patch.benderRange = 12;
		patchTemp->patch.assignMode = defaultAssignMode;
		patchTemp->patch.reverbSwitch = 1;
		patchTemp->patch.dummy = 0;

		patchTemp->outputLevel = 80;
		patchTemp->panpot = controlROMData[controlROMMap->panSettings + settingsIx];
		memset(patchTemp->dummyv, 0, sizeof(patchTemp->dummyv));
		patchTemp->dummyv[1] = 127;

		if (i == RHYTHM_PART_NUM) {
			parts[i] = new RhythmPart(this, i);
		} else {
			parts[i] = new Part(this, i);
			parts[i]->setProgram(controlROMData[controlROMMap->programSettings + settingsIx]);
		}
	}
	refreshSystemReserveSettings();
	refreshSystemChanAssign(0, Bit8u(partCount - 1));

	// The factory patches are not in the control ROM; every D-110 patch starts as the power-on multi setup. The D-20's
	// performance patches start as its initial patches, with A11 current.
	for (unsigned int patchNum = 0; patchNum < 128; patchNum++) {
		initD20Patch(mt32ram.d20Patches[patchNum], patchNum);
	}
	initD20Patch(mt32ram.d20PatchTemp, 0);
	extensions.currentPerformance = 0;
	for (Bit8u patchNum = 0; patchNum < 64; patchNum++) {
		storePatchNow(patchNum);
	}
	extensions.currentPatch = 0;

	// For resetting mt32 mid-execution
	mt32default = mt32ram;

	midiQueue = new MidiEventQueue(extensions.midiEventQueueSize, extensions.midiEventQueueSysexStorageBufferSize);

	analog = Analog::createAnalog(analogOutputMode, controlROMFeatures->oldMT32AnalogLPF, getSelectedRendererType());
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT && extensions.partOutputsAvailable; stream++) {
		if (stream < MAX_PART_COUNT && !extensions.partStreamsAvailable) continue;
		partAnalogs[stream] = Analog::createAnalog(analogOutputMode, controlROMFeatures->oldMT32AnalogLPF, getSelectedRendererType());
	}
#if MT32EMU_MONITOR_INIT
	static const char *ANALOG_OUTPUT_MODES[] = { "Digital only", "Coarse", "Accurate", "Oversampled2x" };
	printDebug("Using Analog output mode %s", ANALOG_OUTPUT_MODES[analogOutputMode]);
#endif
	setOutputGain(outputGain);
	setReverbOutputGain(reverbOutputGain);

	switch (getSelectedRendererType()) {
		case RendererType_BIT16S:
			renderer = new RendererImpl<IntSample>(*this);
#if MT32EMU_MONITOR_INIT
			printDebug("Using integer 16-bit samples in renderer and wave generator");
#endif
			break;
		case RendererType_FLOAT:
			renderer = new RendererImpl<FloatSample>(*this);
#if MT32EMU_MONITOR_INIT
			printDebug("Using float 32-bit samples in renderer and wave generator");
#endif
			break;
		default:
			printDebug("Synth: Unknown renderer type %i\n", getSelectedRendererType());
			dispose();
			return false;
	}

	opened = true;
	activated = false;

#if MT32EMU_MONITOR_INIT
	printDebug("*** Initialisation complete ***");
#endif
	return true;
}

void Synth::dispose() {
	opened = false;

	delete midiQueue;
	midiQueue = NULL;

	delete renderer;
	renderer = NULL;

	delete analog;
	analog = NULL;
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
		delete partAnalogs[stream];
		partAnalogs[stream] = NULL;
	}

	delete partialManager;
	partialManager = NULL;

	for (unsigned int i = 0; i < MAX_PART_COUNT; i++) {
		delete parts[i];
		parts[i] = NULL;
	}
	partCount = BASE_PART_COUNT;

	delete[] soundGroupNames;
	soundGroupNames = NULL;

	delete[] pcmWaves;
	pcmWaves = NULL;

	delete[] pcmROMData;
	pcmROMData = NULL;

	deleteMemoryRegions();

	for (int i = REVERB_MODE_ROOM; i <= REVERB_MODE_TAP_DELAY; i++) {
		delete reverbModels[i];
		reverbModels[i] = NULL;
	}
	delete dSeriesReverbModel;
	dSeriesReverbModel = NULL;
	reverbModel = NULL;
	controlROMFeatures = NULL;
	controlROMMap = NULL;

	delete[] controlROMData;
	controlROMData = NULL;
	controlROMSize = 0;
}

void Synth::close() {
	if (opened) {
		dispose();
	}
}

bool Synth::isOpen() const {
	return opened;
}

void Synth::flushMIDIQueue() {
	if (midiQueue == NULL) return;
	for (;;) {
		const volatile MidiEventQueue::MidiEvent *midiEvent = midiQueue->peekMidiEvent();
		if (midiEvent == NULL) break;
		if (midiEvent->sysexData == NULL) {
			playMsgNow(midiEvent->shortMessageData);
		} else {
			playSysexNow(midiEvent->sysexData, midiEvent->sysexLength);
		}
		midiQueue->dropMidiEvent();
	}
	lastReceivedMIDIEventTimestamp = renderedSampleCount;
}

Bit32u Synth::setMIDIEventQueueSize(Bit32u useSize) {
	static const Bit32u MAX_QUEUE_SIZE = (1 << 24); // This results in about 256 Mb - much greater than any reasonable value

	if (extensions.midiEventQueueSize == useSize) return useSize;

	// Find a power of 2 that is >= useSize
	Bit32u binarySize = 1;
	if (useSize < MAX_QUEUE_SIZE) {
		// Using simple linear search as this isn't time critical
		while (binarySize < useSize) binarySize <<= 1;
	} else {
		binarySize = MAX_QUEUE_SIZE;
	}
	extensions.midiEventQueueSize = binarySize;
	if (midiQueue != NULL) {
		flushMIDIQueue();
		delete midiQueue;
		midiQueue = new MidiEventQueue(binarySize, extensions.midiEventQueueSysexStorageBufferSize);
	}
	return binarySize;
}

void Synth::configureMIDIEventQueueSysexStorage(Bit32u storageBufferSize) {
	if (extensions.midiEventQueueSysexStorageBufferSize == storageBufferSize) return;

	extensions.midiEventQueueSysexStorageBufferSize = storageBufferSize;
	if (midiQueue != NULL) {
		flushMIDIQueue();
		delete midiQueue;
		midiQueue = new MidiEventQueue(extensions.midiEventQueueSize, storageBufferSize);
	}
}

Bit32u Synth::getShortMessageLength(Bit32u msg) {
	if ((msg & 0xF0) == 0xF0) {
		switch (msg & 0xFF) {
			case 0xF1:
			case 0xF3:
				return 2;
			case 0xF2:
				return 3;
			default:
				return 1;
		}
	}
	// NOTE: This calculation isn't quite correct
	// as it doesn't consider the running status byte
	return ((msg & 0xE0) == 0xC0) ? 2 : 3;
}

Bit32u Synth::addMIDIInterfaceDelay(Bit32u len, Bit32u timestamp) {
	Bit32u transferTime =  Bit32u(double(len) * MIDI_DATA_TRANSFER_RATE);
	// Dealing with wrapping
	if (Bit32s(timestamp - lastReceivedMIDIEventTimestamp) < 0) {
		timestamp = lastReceivedMIDIEventTimestamp;
	}
	timestamp += transferTime;
	lastReceivedMIDIEventTimestamp = timestamp;
	return timestamp;
}

Bit32u Synth::getInternalRenderedSampleCount() const {
	return renderedSampleCount;
}

bool Synth::playMsg(Bit32u msg) {
	return playMsg(msg, renderedSampleCount);
}

bool Synth::playMsg(Bit32u msg, Bit32u timestamp) {
	if ((msg & 0xF8) == 0xF8) {
		reportHandler->onMIDISystemRealtime(Bit8u(msg & 0xFF));
		return true;
	}
	if (midiQueue == NULL) return false;
	if (midiDelayMode != MIDIDelayMode_IMMEDIATE) {
		timestamp = addMIDIInterfaceDelay(getShortMessageLength(msg), timestamp);
	}
	if (!activated) activated = true;
	do {
		if (midiQueue->pushShortMessage(msg, timestamp)) return true;
	} while (reportHandler->onMIDIQueueOverflow());
	return false;
}

bool Synth::playSysex(const Bit8u *sysex, Bit32u len) {
	return playSysex(sysex, len, renderedSampleCount);
}

bool Synth::playSysex(const Bit8u *sysex, Bit32u len, Bit32u timestamp) {
	if (midiQueue == NULL) return false;
	if (midiDelayMode == MIDIDelayMode_DELAY_ALL) {
		timestamp = addMIDIInterfaceDelay(len, timestamp);
	}
	if (!activated) activated = true;
	do {
		if (midiQueue->pushSysex(sysex, len, timestamp)) return true;
	} while (reportHandler->onMIDIQueueOverflow());
	return false;
}

void Synth::playMsgNow(Bit32u msg) {
	if (!opened) return;

	// NOTE: Active sense IS implemented in real hardware. However, realtime processing is clearly out of the library scope.
	//       It is assumed that realtime consumers of the library respond to these MIDI events as appropriate.

	Bit8u code = Bit8u((msg & 0x0000F0) >> 4);
	Bit8u chan = Bit8u(msg & 0x00000F);
	Bit8u note = Bit8u((msg & 0x007F00) >> 8);
	Bit8u velocity = Bit8u((msg & 0x7F0000) >> 16);

	//printDebug("Playing chan %d, code 0x%01x note: 0x%02x", chan, code, note);

	if (code == 0xC && extensions.performanceMode && chan == extensions.performanceChannel) {
		// D-20 performance mode: program changes on the performance channel select performance patches.
		recallPerformanceNow(note);
		reportHandler->onMIDIMessagePlayed();
		return;
	}
	if (code == 0xC && chan == extensions.controlChannel && isD110()) {
		// D-110: program changes on the control channel select patches, even where parts listen on it.
		// Program numbers 64-127 address card patches; without a card the internal patch is used.
		recallPatchNow(Bit8u(note & 0x7F));
		reportHandler->onMIDIMessagePlayed();
		return;
	}

	Bit8u *chanParts = extensions.chantable[chan];
	if (*chanParts >= MAX_PART_COUNT) {
#if MT32EMU_MONITOR_MIDI > 0
		printDebug("Play msg on unreg chan %d (%d): code=0x%01x, vel=%d", chan, *chanParts, code, velocity);
#endif
		return;
	}
	for (Bit32u i = extensions.abortingPartIx; i < MAX_PART_COUNT; i++) {
		const Bit32u partNum = chanParts[i];
		if (partNum >= MAX_PART_COUNT) break;
		playMsgOnPart(partNum, code, note, velocity);
		if (isAbortingPoly()) {
			extensions.abortingPartIx = i;
			break;
		} else if (extensions.abortingPartIx) {
			extensions.abortingPartIx = 0;
		}
	}
}

void Synth::playMsgOnPart(Bit8u part, Bit8u code, Bit8u note, Bit8u velocity) {
	if (!opened) return;

	Bit32u bend;

	if (!activated) activated = true;
	//printDebug("Synth::playMsgOnPart(%02x, %02x, %02x, %02x)", part, code, note, velocity);
	switch (code) {
	case 0x8:
		//printDebug("Note OFF - Part %d", part);
		// The MT-32 ignores velocity for note off
		parts[part]->noteOff(note);
		break;
	case 0x9:
		//printDebug("Note ON - Part %d, Note %d Vel %d", part, note, velocity);
		if (velocity == 0) {
			// MIDI defines note-on with velocity 0 as being the same as note-off with velocity 40
			parts[part]->noteOff(note);
		} else {
			parts[part]->noteOn(note, velocity);
		}
		break;
	case 0xB: // Control change
		switch (note) {
		case 0x01:  // Modulation
			//printDebug("Modulation: %d", velocity);
			parts[part]->setModulation(velocity);
			break;
		case 0x05: // Portamento time (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setPortamentoTime(velocity);
			break;
		case 0x06:
			parts[part]->setDataEntryMSB(velocity);
			break;
		case 0x26: // Data entry LSB (MIDI extension: the cents of the pitch bend range)
			if (!extensions.midiExtensions) return;
			parts[part]->setDataEntryLSB(velocity);
			break;
		case 0x07:  // Set volume
			//printDebug("Volume set: %d", velocity);
			parts[part]->setVolume(velocity);
			break;
		case 0x0A:  // Pan
			//printDebug("Pan set: %d", velocity);
			parts[part]->setPan(velocity);
			break;
		case 0x0B:
			//printDebug("Expression set: %d", velocity);
			parts[part]->setExpression(velocity);
			break;
		case 0x40: // Hold (sustain) pedal
			//printDebug("Hold pedal set: %d", velocity);
			parts[part]->setHoldPedal(velocity >= 64);
			break;
		case 0x41: // Portamento on/off (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setPortamento(velocity >= 64);
			break;

		case 0x47: // Harmonic content (MIDI extension: resonance)
			if (!extensions.midiExtensions) return;
			parts[part]->setHarmonicContent(velocity);
			break;
		case 0x48: // Release time (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setReleaseTime(velocity);
			break;
		case 0x49: // Attack time (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setAttackTime(velocity);
			break;
		case 0x4A: // Brightness (MIDI extension: cutoff)
			if (!extensions.midiExtensions) return;
			parts[part]->setBrightness(velocity);
			break;
		case 0x4B: // Decay time (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setDecayTime(velocity);
			break;
		case 0x4C: // Vibrato rate (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setVibratoRate(velocity);
			break;
		case 0x4D: // Vibrato depth (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setVibratoDepth(velocity);
			break;
		case 0x54: // Portamento control: the key the next note glides from (MIDI extension)
			if (!extensions.midiExtensions) return;
			parts[part]->setPortamentoControl(velocity);
			break;

		case 0x62:
			parts[part]->setNRPNLSB(velocity);
			break;
		case 0x63:
			parts[part]->setNRPNMSB(velocity);
			break;
		case 0x64:
			parts[part]->setRPNLSB(velocity);
			break;
		case 0x65:
			parts[part]->setRPNMSB(velocity);
			break;

		case 0x79: // Reset all controllers
			//printDebug("Reset all controllers");
			parts[part]->resetAllControllers();
			break;

		case 0x7B: // All notes off
			//printDebug("All notes off");
			parts[part]->allNotesOff();
			break;

		case 0x7C:
		case 0x7D:
		case 0x7E:
		case 0x7F:
			// CONFIRMED:Mok: A real LAPC-I responds to these controllers as follows:
			parts[part]->setHoldPedal(false);
			parts[part]->allNotesOff();
			break;

		default:
#if MT32EMU_MONITOR_MIDI > 0
			printDebug("Unknown MIDI Control code: 0x%02x - vel 0x%02x", note, velocity);
#endif
			return;
		}

		break;
	case 0xC: // Program change
		//printDebug("Program change %01x", note);
		parts[part]->setProgram(note);
		break;
	case 0xE: // Pitch bender
		bend = (velocity << 7) | (note);
		//printDebug("Pitch bender %02x", bend);
		parts[part]->setBend(bend);
		break;
	default:
#if MT32EMU_MONITOR_MIDI > 0
		printDebug("Unknown Midi code: 0x%01x - %02x - %02x", code, note, velocity);
#endif
		return;
	}
	reportHandler->onMIDIMessagePlayed();
}

void Synth::playSysexNow(const Bit8u *sysex, Bit32u len) {
	if (len < 2) {
		printDebug("playSysex: Message is too short for sysex (%d bytes)", len);
	}
	if (sysex[0] != 0xF0) {
		printDebug("playSysex: Message lacks start-of-sysex (0xF0)");
		return;
	}
	// Due to some programs (e.g. Java) sending buffers with junk at the end, we have to go through and find the end marker rather than relying on len.
	Bit32u endPos;
	for (endPos = 1; endPos < len; endPos++) {
		if (sysex[endPos] == 0xF7) {
			break;
		}
	}
	if (endPos == len) {
		printDebug("playSysex: Message lacks end-of-sysex (0xf7)");
		return;
	}
	playSysexWithoutFraming(sysex + 1, endPos - 1);
}

// GM/GM2 System On (7E xx 09 01/03), GS Reset and SC-88 mode set (41 xx 42 12 40 00 7F / 00 00 7F),
// XG System On (43 1x 4C 00 00 7E): messages that ask a GM-style module to reset its channels.
static bool isMIDIResetSysex(const Bit8u *sysex, Bit32u len) {
	if (len >= 4 && sysex[0] == 0x7E && sysex[2] == 0x09 && (sysex[3] == 0x01 || sysex[3] == 0x03)) return true;
	if (len >= 8 && sysex[0] == SYSEX_MANUFACTURER_ROLAND && sysex[2] == 0x42 && sysex[3] == SYSEX_CMD_DT1 &&
		(sysex[4] == 0x40 || sysex[4] == 0x00) && sysex[5] == 0x00 && sysex[6] == 0x7F) return true;
	if (len >= 7 && sysex[0] == 0x43 && (sysex[1] & 0xF0) == 0x10 && sysex[2] == 0x4C && sysex[3] == 0x00 &&
		sysex[4] == 0x00 && sysex[5] == 0x7E) return true;
	return false;
}

bool Synth::isMIDIResetMessage(const Bit8u *sysex, Bit32u len) {
	return len >= 2 && sysex[0] == 0xF0 && isMIDIResetSysex(sysex + 1, len - 1);
}

// GS MASTER TUNE: 41 dev 42 12 40 00 00 and 4 nibbles, 0018H-07E8H = -100.0..+100.0 cents in tenths, 0400H = 0 (the
// SC-88Pro's MIDI implementation). Returns the tuning in tenths of a cent, or false for any other message.
static bool parseGSMasterTune(const Bit8u *sysex, Bit32u len, Bit32s &tenthsOfCent) {
	if (len < 12 || sysex[0] != SYSEX_MANUFACTURER_ROLAND || sysex[2] != 0x42 || sysex[3] != SYSEX_CMD_DT1) return false;
	if (sysex[4] != 0x40 || sysex[5] != 0x00 || sysex[6] != 0x00) return false;
	Bit32u sum = 0;
	for (Bit32u i = 4; i < len; i++) sum += sysex[i]; // Address, data and checksum
	if ((sum & 0x7F) != 0) return false; // Damaged: a GS unit ignores it
	Bit32s value = 0;
	for (Bit32u i = 7; i < 11; i++) value = (value << 4) | (sysex[i] & 0x0F);
	if (value < 0x18) value = 0x18;
	if (value > 0x7E8) value = 0x7E8;
	tenthsOfCent = value - 0x400;
	return true;
}

void Synth::playSysexWithoutFraming(const Bit8u *sysex, Bit32u len) {
	if (len < 4) {
		printDebug("playSysexWithoutFraming: Message is too short (%d bytes)!", len);
		return;
	}
	if (extensions.midiExtensions && isMIDIResetSysex(sysex, len)) {
		resetMIDIChannels();
		resetPartLevelsAndPans();
		return;
	}
	Bit32s gsTune;
	if (extensions.midiExtensions && parseGSMasterTune(sysex, len, gsTune)) {
		setGSMasterTune(gsTune);
		return;
	}
	if (sysex[0] != SYSEX_MANUFACTURER_ROLAND) {
		printDebug("playSysexWithoutFraming: Header not intended for this device manufacturer: %02x %02x %02x %02x", int(sysex[0]), int(sysex[1]), int(sysex[2]), int(sysex[3]));
		return;
	}
	if (sysex[2] == SYSEX_MDL_D50) {
		printDebug("playSysexWithoutFraming: Header is intended for model D-50 (not yet supported): %02x %02x %02x %02x", int(sysex[0]), int(sysex[1]), int(sysex[2]), int(sysex[3]));
		return;
	} else if (sysex[2] != SYSEX_MDL_MT32) {
		printDebug("playSysexWithoutFraming: Header not intended for model MT-32: %02x %02x %02x %02x", int(sysex[0]), int(sysex[1]), int(sysex[2]), int(sysex[3]));
		return;
	}
	playSysexWithoutHeader(sysex[1], sysex[3], sysex + 4, len - 4);
}

void Synth::playSysexWithoutHeader(Bit8u device, Bit8u command, const Bit8u *sysex, Bit32u len) {
	if (device >= 0x10 && device != extensions.deviceID) {
		// We have device ID 0x10 (default, but changeable, on real MT-32), < 0x10 is for channels
		printDebug("playSysexWithoutHeader: Message is not intended for this device ID (provided: %02x, expected: %02x or channel)", int(device), int(extensions.deviceID));
		return;
	}
	// This is checked early in the real devices (before any sysex length checks or further processing)
	// FIXME: Response to SYSEX_CMD_DAT reset with partials active (and in general) is untested.
	// The D-110 has no reset address: 7F xx xx falls through to an unmapped write.
	if ((command == SYSEX_CMD_DT1 || command == SYSEX_CMD_DAT) && sysex[0] == 0x7F && !isD110()) {
		reset();
		return;
	}

	if (command == SYSEX_CMD_EOD) {
#if MT32EMU_MONITOR_SYSEX > 0
		printDebug("playSysexWithoutHeader: Ignored unsupported command %02x", command);
#endif
		return;
	}
	if (len < 4) {
		printDebug("playSysexWithoutHeader: Message is too short (%d bytes)!", len);
		return;
	}
	Bit8u checksum = calcSysexChecksum(sysex, len - 1);
	if (checksum != sysex[len - 1]) {
		printDebug("playSysexWithoutHeader: Message checksum is incorrect (provided: %02x, expected: %02x)!", sysex[len - 1], checksum);
		return;
	}
	len -= 1; // Exclude checksum
	switch (command) {
	case SYSEX_CMD_WSD:
#if MT32EMU_MONITOR_SYSEX > 0
		printDebug("playSysexWithoutHeader: Ignored unsupported command %02x", command);
#endif
		break;
	case SYSEX_CMD_DAT:
		/* Outcommented until we (ever) actually implement handshake communication
		if (hasActivePartials()) {
			printDebug("playSysexWithoutHeader: Got SYSEX_CMD_DAT but partials are active - ignoring");
			// FIXME: We should send SYSEX_CMD_RJC in this case
			break;
		}
		*/
		// Fall-through
	case SYSEX_CMD_DT1:
		writeSysex(device, sysex, len);
		break;
	case SYSEX_CMD_RQD:
		if (hasActivePartials()) {
			printDebug("playSysexWithoutHeader: Got SYSEX_CMD_RQD but partials are active - ignoring");
			// FIXME: We should send SYSEX_CMD_RJC in this case
			break;
		}
		// Fall-through
	case SYSEX_CMD_RQ1:
		readSysex(device, sysex, len);
		break;
	default:
		printDebug("playSysexWithoutHeader: Unsupported command %02x", command);
		return;
	}
}

void Synth::readSysex(Bit8u /*device*/, const Bit8u * /*sysex*/, Bit32u /*len*/) const {
	// NYI
}

void Synth::writeSysex(Bit8u device, const Bit8u *sysex, Bit32u len) {
	if (!opened) return;
	reportHandler->onMIDIMessagePlayed();
	Bit32u addr = (sysex[0] << 16) | (sysex[1] << 8) | (sysex[2]);
	addr = MT32EMU_MEMADDR(addr);
	sysex += 3;
	len -= 3;
	//printDebug("Sysex addr: 0x%06x", MT32EMU_SYSEXMEMADDR(addr));
	// NOTE: Please keep both lower and upper bounds in each check, for ease of reading

	// Process channel-specific sysex by converting it to device-global
	if (device < 0x10) {
#if MT32EMU_MONITOR_SYSEX > 0
		printDebug("WRITE-CHANNEL: Channel %d temp area 0x%06x", device, MT32EMU_SYSEXMEMADDR(addr));
#endif
		if (/*addr >= MT32EMU_MEMADDR(0x000000) && */addr < MT32EMU_MEMADDR(0x010000)) {
			Bit8u *chanParts = extensions.chantable[device];
			if (*chanParts >= MAX_PART_COUNT) {
#if MT32EMU_MONITOR_SYSEX > 0
				printDebug(" (Channel not mapped to a part... 0 offset)");
#endif
				addr += MT32EMU_MEMADDR(0x030000);
			} else {
				for (Bit32u partIx = 0; partIx < MAX_PART_COUNT; partIx++) {
					if (chanParts[partIx] >= MAX_PART_COUNT) break;
					// The rhythm part is addressed like part 1 (0 offset), as upstream.
					const unsigned int partNum = chanParts[partIx] == RHYTHM_PART_NUM ? 0 : chanParts[partIx];
					writeSysexGlobal(addr + partTempAddress(partNum), sysex, len);
				}
				return;
			}
		} else if (/*addr >= MT32EMU_MEMADDR(0x010000) && */ addr < MT32EMU_MEMADDR(0x020000)) {
			addr += MT32EMU_MEMADDR(0x030110) - MT32EMU_MEMADDR(0x010000);
		} else if (/*addr >= MT32EMU_MEMADDR(0x020000) && */ addr < MT32EMU_MEMADDR(0x030000)) {
			addr -= MT32EMU_MEMADDR(0x020000);
			Bit8u *chanParts = extensions.chantable[device];
			if (*chanParts >= MAX_PART_COUNT) {
#if MT32EMU_MONITOR_SYSEX > 0
				printDebug(" (Channel not mapped to a part... 0 offset)");
#endif
				addr += MT32EMU_MEMADDR(0x040000);
			} else {
				for (Bit32u partIx = 0; partIx < MAX_PART_COUNT; partIx++) {
					if (chanParts[partIx] >= MAX_PART_COUNT) break;
					const unsigned int partNum = chanParts[partIx] == RHYTHM_PART_NUM ? 0 : chanParts[partIx];
					writeSysexGlobal(addr + partToneTempAddress(partNum), sysex, len);
				}
				return;
			}
		} else {
#if MT32EMU_MONITOR_SYSEX > 0
			printDebug(" Invalid channel");
#endif
			return;
		}
	}
	writeSysexGlobal(addr, sysex, len);
}

// Process device-global sysex (possibly converted from channel-specific sysex above)
void Synth::writeSysexGlobal(Bit32u addr, const Bit8u *sysex, Bit32u len) {
	for (;;) {
		// Find the appropriate memory region
		const MemoryRegion *region = findMemoryRegion(addr);

		if (region == NULL) {
			printDebug("Sysex write to unrecognised address %06x, len %d", MT32EMU_SYSEXMEMADDR(addr), len);
			break;
		}
		if (region == systemMemoryRegion && isD110() && addr + len > region->regionEnd()) {
			// Only a D-20 writes past the D-110 system area. Its bytes 0D-20 are dummies for the D-110's part
			// channels and patch name, so those are kept; its part levels and pans follow in the next region.
			const Bit32u off = Bit32u(region->offset(addr));
			if (off < SYSTEM_CHAN_ASSIGN_START_OFF) {
				writeMemoryRegion(region, addr, SYSTEM_CHAN_ASSIGN_START_OFF - off, sysex);
			}
			const Bit32u skip = region->regionEnd() - addr;
			addr += skip;
			sysex += skip;
			len -= skip;
			continue;
		}
		writeMemoryRegion(region, addr, region->getClampedLen(addr, len), sysex);

		Bit32u next = region->next(addr, len);
		if (next == 0) {
			break;
		}
		addr += next;
		sysex += next;
		len -= next;
	}
}

void Synth::readMemory(Bit32u addr, Bit32u len, Bit8u *data) {
	if (!opened) return;
	const MemoryRegion *region = findMemoryRegion(addr);
	if (region != NULL) {
		readMemoryRegion(region, addr, len, data);
	}
}

void Synth::initMemoryRegions() {
	// Timbre max tables are slightly more complicated than the others, which are used directly from the ROM.
	// The ROM (sensibly) just has maximums for TimbreParam.commonParam followed by just one TimbreParam.partialParam,
	// so we produce a table with all partialParams filled out, as well as padding for PaddedTimbre, for quick lookup.
	paddedTimbreMaxTable = new Bit8u[sizeof(MemParams::PaddedTimbre)];
	memcpy(&paddedTimbreMaxTable[0], &controlROMData[controlROMMap->timbreMaxTable], sizeof(TimbreParam::CommonParam) + sizeof(TimbreParam::PartialParam)); // commonParam and one partialParam
	int pos = sizeof(TimbreParam::CommonParam) + sizeof(TimbreParam::PartialParam);
	for (int i = 0; i < 3; i++) {
		memcpy(&paddedTimbreMaxTable[pos], &controlROMData[controlROMMap->timbreMaxTable + sizeof(TimbreParam::CommonParam)], sizeof(TimbreParam::PartialParam));
		pos += sizeof(TimbreParam::PartialParam);
	}
	memset(&paddedTimbreMaxTable[pos], 0, 10); // Padding
	patchTempMemoryRegion = new PatchTempMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.patchTemp[0]), &controlROMData[controlROMMap->patchMaxTable]);
	rhythmTempMemoryRegion = new RhythmTempMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.rhythmTemp[0]), &controlROMData[controlROMMap->rhythmMaxTable]);
	timbreTempMemoryRegion = new TimbreTempMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.timbreTemp[0]), paddedTimbreMaxTable);
	patchesMemoryRegion = new PatchesMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.patches[0]), &controlROMData[controlROMMap->patchMaxTable]);
	timbresMemoryRegion = new TimbresMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.timbres[0]), paddedTimbreMaxTable);
	systemMemoryRegion = new SystemMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.system), &controlROMData[controlROMMap->systemMaxTable], isD110() ? SYSTEM_SIZE_D110 : SYSTEM_SIZE_MT32);
	displayMemoryRegion = new DisplayMemoryRegion(this);
	if (isD110()) {
		// The D-110 has no reset address (7F 00 00), but patch memory, write requests and, for D-20 dumps,
		// the D-20's performance patches, rhythm setup memory and the tail of its system area.
		d110PatchesMemoryRegion = new D110PatchesMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.d110Patches[0]));
		d20PatchesMemoryRegion = new D20PatchesMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.d20Patches[0]));
		static const Bit8u D20_PATCH_MAX[D20_PATCH_SIZE] = {2, 61, 3, 63, 3, 63, 48, 48, 100, 100, 24, 24, 3, 3, 1, 1, 8, 7, 7, 100, 100,
			127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 0};
		d20PatchTempMemoryRegion = new D20PatchTempMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.d20PatchTemp), const_cast<Bit8u *>(D20_PATCH_MAX));
		cardTonesMemoryRegion = new CardTonesMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.timbres[CARD_TONE_BASE]), paddedTimbreMaxTable);
		cardTimbresMemoryRegion = new CardTimbresMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.cardTimbres[0]), &controlROMData[controlROMMap->patchMaxTable]);
		cardPatchesMemoryRegion = new CardPatchesMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.cardPatches[0]));
		d20RhythmSetupMemoryRegion = new D20RhythmSetupMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.rhythmTemp[0]), &controlROMData[controlROMMap->rhythmMaxTable]);
		d20SystemExtMemoryRegion = new D20SystemExtMemoryRegion(this);
		writeRequestMemoryRegion = new WriteRequestMemoryRegion(this);
		static const Bit8u FINE_PAN_MAX[sizeof(MemParams::FinePan)] = {1, 127};
		partFinePanMemoryRegion = new PartFinePanMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.partFinePan[0]), const_cast<Bit8u *>(FINE_PAN_MAX));
		rhythmFinePanMemoryRegion = new RhythmFinePanMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.rhythmFinePan[0]), const_cast<Bit8u *>(FINE_PAN_MAX));
		d20PatternsMemoryRegion = new D20PatternsMemoryRegion(this, &mt32ram.d20Patterns[32][0], MR_D20Patterns, MT32EMU_MEMADDR(0x0A0000));
		d20PresetPatternsMemoryRegion = new D20PatternsMemoryRegion(this, &mt32ram.d20Patterns[0][0], MR_D20PresetPatterns, MT32EMU_MEMADDR(0x0D0000));
		d20RhythmTrackMemoryRegion = new D20RhythmTrackMemoryRegion(this, &mt32ram.d20RhythmTrack[0]);
		if (partCount > BASE_PART_COUNT) {
			static const Bit8u SYSTEM_EXT_MAX[sizeof(MemParams::SystemExt)] = {32, 32, 32, 32, 32, 32, 32, 0, 16, 16, 16, 16, 16, 16, 16, 0};
			extPatchTempMemoryRegion = new ExtPatchTempMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.patchTemp[BASE_PART_COUNT]), &controlROMData[controlROMMap->patchMaxTable]);
			extTimbreTempMemoryRegion = new ExtTimbreTempMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.timbreTemp[RHYTHM_PART_NUM]), paddedTimbreMaxTable);
			extSystemMemoryRegion = new ExtSystemMemoryRegion(this, reinterpret_cast<Bit8u *>(&mt32ram.systemExt), const_cast<Bit8u *>(SYSTEM_EXT_MAX));
		}
	} else {
		resetMemoryRegion = new ResetMemoryRegion(this);
	}
}

void Synth::deleteMemoryRegions() {
	delete patchTempMemoryRegion;
	patchTempMemoryRegion = NULL;
	delete rhythmTempMemoryRegion;
	rhythmTempMemoryRegion = NULL;
	delete timbreTempMemoryRegion;
	timbreTempMemoryRegion = NULL;
	delete patchesMemoryRegion;
	patchesMemoryRegion = NULL;
	delete timbresMemoryRegion;
	timbresMemoryRegion = NULL;
	delete systemMemoryRegion;
	systemMemoryRegion = NULL;
	delete displayMemoryRegion;
	displayMemoryRegion = NULL;
	delete resetMemoryRegion;
	resetMemoryRegion = NULL;
	delete d110PatchesMemoryRegion;
	d110PatchesMemoryRegion = NULL;
	delete d20PatchesMemoryRegion;
	d20PatchesMemoryRegion = NULL;
	delete d20PatchTempMemoryRegion;
	d20PatchTempMemoryRegion = NULL;
	delete cardTonesMemoryRegion;
	cardTonesMemoryRegion = NULL;
	delete cardTimbresMemoryRegion;
	cardTimbresMemoryRegion = NULL;
	delete cardPatchesMemoryRegion;
	cardPatchesMemoryRegion = NULL;
	delete partFinePanMemoryRegion;
	partFinePanMemoryRegion = NULL;
	delete rhythmFinePanMemoryRegion;
	rhythmFinePanMemoryRegion = NULL;
	delete d20PatternsMemoryRegion;
	d20PatternsMemoryRegion = NULL;
	delete d20PresetPatternsMemoryRegion;
	d20PresetPatternsMemoryRegion = NULL;
	delete d20RhythmTrackMemoryRegion;
	d20RhythmTrackMemoryRegion = NULL;
	delete d20RhythmSetupMemoryRegion;
	d20RhythmSetupMemoryRegion = NULL;
	delete d20SystemExtMemoryRegion;
	d20SystemExtMemoryRegion = NULL;
	delete writeRequestMemoryRegion;
	writeRequestMemoryRegion = NULL;
	delete extPatchTempMemoryRegion;
	extPatchTempMemoryRegion = NULL;
	delete extTimbreTempMemoryRegion;
	extTimbreTempMemoryRegion = NULL;
	delete extSystemMemoryRegion;
	extSystemMemoryRegion = NULL;

	delete[] paddedTimbreMaxTable;
	paddedTimbreMaxTable = NULL;
}

MemoryRegion *Synth::findMemoryRegion(Bit32u addr) {
	MemoryRegion *regions[] = {
		patchTempMemoryRegion,
		rhythmTempMemoryRegion,
		timbreTempMemoryRegion,
		patchesMemoryRegion,
		timbresMemoryRegion,
		systemMemoryRegion,
		displayMemoryRegion,
		resetMemoryRegion,
		d110PatchesMemoryRegion,
		d20PatchesMemoryRegion,
		d20PatchTempMemoryRegion,
		cardTonesMemoryRegion,
		cardTimbresMemoryRegion,
		cardPatchesMemoryRegion,
		d20RhythmSetupMemoryRegion,
		d20SystemExtMemoryRegion,
		writeRequestMemoryRegion,
		extPatchTempMemoryRegion,
		extTimbreTempMemoryRegion,
		extSystemMemoryRegion,
		partFinePanMemoryRegion,
		rhythmFinePanMemoryRegion,
		d20PatternsMemoryRegion,
		d20PresetPatternsMemoryRegion,
		d20RhythmTrackMemoryRegion
	};
	for (unsigned int pos = 0; pos < sizeof(regions) / sizeof(regions[0]); pos++) {
		if (regions[pos] != NULL && regions[pos]->contains(addr)) {
			return regions[pos];
		}
	}
	return NULL;
}

void Synth::readMemoryRegion(const MemoryRegion *region, Bit32u addr, Bit32u len, Bit8u *data) {
	unsigned int first = region->firstTouched(addr);
	//unsigned int last = region->lastTouched(addr, len);
	unsigned int off = region->firstTouchedOffset(addr);
	len = region->getClampedLen(addr, len);
	if (region->type == MR_Timbres) {
		// SysEx address 08 00 00 is the first memory timbre, stored after groups A and B (as when writing).
		first += 128;
	}

	unsigned int m;

	if (region->isReadable()) {
		region->read(first, off, data, len);
	} else {
		// FIXME: We might want to do these properly in future
		for (m = 0; m < len; m += 2) {
			data[m] = 0xff;
			if (m + 1 < len) {
				data[m+1] = Bit8u(region->type);
			}
		}
	}
}

void Synth::writeMemoryRegion(const MemoryRegion *region, Bit32u addr, Bit32u len, const Bit8u *data) {
	unsigned int first = region->firstTouched(addr);
	unsigned int last = region->lastTouched(addr, len);
	unsigned int off = region->firstTouchedOffset(addr);
	switch (region->type) {
	case MR_PatchTemp:
	case MR_ExtPatchTemp:
		region->write(first, off, data, len);
		//printDebug("Patch temp: Patch %d, offset %x, len %d", off/16, off % 16, len);

		for (unsigned int i = first; i <= last; i++) {
			const unsigned int panOff = i * region->entrySize + offsetof(MemParams::PatchTemp, panpot);
			if (panOff >= first * region->entrySize + off && panOff < first * region->entrySize + off + len) {
				clearFinePan(false, region->type == MR_ExtPatchTemp ? i + BASE_PART_COUNT : i);
			}
			// Note: Confirmed on CM-64 that we definitely *should* update the timbre here,
			// but only in the case that the sysex actually writes to those values
			// (D-110: the card flag at offset 7 picks the tones of group i/c too).
			const bool cardFlagTouched = i == first && off <= 7 && off + len > 7;
			refreshPartTemp(region->type == MR_ExtPatchTemp ? i + BASE_PART_COUNT : i, !(i == first && off > 2) || cardFlagTouched);
		}
		break;
	case MR_CardTones:
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) {
			for (unsigned int part = 0; part < partCount; part++) {
				if (parts[part] != NULL) parts[part]->refreshTimbre(CARD_TONE_BASE + i);
			}
		}
		break;
	case MR_CardTimbres:
	case MR_CardPatches:
		region->write(first, off, data, len);
		break;
	case MR_D20PatchTemp:
		region->write(0, off, data, len);
		if (extensions.performanceMode) {
			applyPerformanceNow();
		}
		break;
	case MR_ExtTimbreTemp:
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) {
			if (parts[i + BASE_PART_COUNT] != NULL) {
				parts[i + BASE_PART_COUNT]->refresh();
			}
		}
		break;
	case MR_ExtSystem: {
		region->write(0, off, data, len);
		// Partial reserves at 00-06, MIDI channels at 08-0E
		static const unsigned int CHAN_OFF = offsetof(MemParams::SystemExt, chanAssign);
		if (off < EXTRA_PART_COUNT) {
			refreshSystemReserveSettings();
		}
		if (off + len > CHAN_OFF && off < CHAN_OFF + EXTRA_PART_COUNT) {
			const unsigned int firstChan = off > CHAN_OFF ? off - CHAN_OFF : 0;
			const unsigned int lastChan = (off + len < CHAN_OFF + EXTRA_PART_COUNT ? off + len : CHAN_OFF + EXTRA_PART_COUNT) - 1 - CHAN_OFF;
			refreshSystemChanAssign(Bit8u(BASE_PART_COUNT + firstChan), Bit8u(BASE_PART_COUNT + lastChan));
		}
		break;
	}
	case MR_RhythmTemp:
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) {
			const unsigned int panOff = i * region->entrySize + offsetof(MemParams::RhythmTemp, panpot);
			if (panOff >= first * region->entrySize + off && panOff < first * region->entrySize + off + len) clearFinePan(true, i);
			int timbreNum = mt32ram.rhythmTemp[i].timbre;
			char timbreName[11];
			if (timbreNum < 94) {
				memcpy(timbreName, mt32ram.timbres[128 + timbreNum].timbre.common.name, 10);
				timbreName[10] = 0;
			} else {
				strcpy(timbreName, "[None]");
			}
#if MT32EMU_MONITOR_SYSEX > 0
			printDebug("WRITE-RHYTHM (%d-%d@%d..%d): %d; level=%02x, panpot=%02x, reverb=%02x, timbre=%d (%s)", first, last, off, off + len, i, mt32ram.rhythmTemp[i].outputLevel, mt32ram.rhythmTemp[i].panpot, mt32ram.rhythmTemp[i].reverbSwitch, mt32ram.rhythmTemp[i].timbre, timbreName);
#endif
		}
		if (parts[8] != NULL) {
			parts[8]->refresh();
		}
		break;
	case MR_TimbreTemp:
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) {
			char instrumentName[11];
			memcpy(instrumentName, mt32ram.timbreTemp[i].common.name, 10);
			instrumentName[10] = 0;
#if MT32EMU_MONITOR_SYSEX > 0
			printDebug("WRITE-PARTTIMBRE (%d-%d@%d..%d): timbre=%d (%s)", first, last, off, off + len, i, instrumentName);
#endif
			if (parts[i] != NULL) {
				parts[i]->refresh();
			}
		}
		break;
	case MR_Patches:
		region->write(first, off, data, len);
#if MT32EMU_MONITOR_SYSEX > 0
		for (unsigned int i = first; i <= last; i++) {
			PatchParam *patch = &mt32ram.patches[i];
			int patchAbsTimbreNum = patch->timbreGroup * 64 + patch->timbreNum;
			char instrumentName[11];
			memcpy(instrumentName, mt32ram.timbres[patchAbsTimbreNum].timbre.common.name, 10);
			instrumentName[10] = 0;
			Bit8u *n = reinterpret_cast<Bit8u *>(patch);
			printDebug("WRITE-PATCH (%d-%d@%d..%d): %d; timbre=%d (%s) %02X%02X%02X%02X%02X%02X%02X%02X", first, last, off, off + len, i, patchAbsTimbreNum, instrumentName, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
		}
#endif
		break;
	case MR_Timbres:
		// Timbres
		first += 128;
		last += 128;
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) {
#if MT32EMU_MONITOR_TIMBRES >= 1
			TimbreParam *timbre = &mt32ram.timbres[i].timbre;
			char instrumentName[11];
			memcpy(instrumentName, timbre->common.name, 10);
			instrumentName[10] = 0;
			printDebug("WRITE-TIMBRE (%d-%d@%d..%d): %d; name=\"%s\"", first, last, off, off + len, i, instrumentName);
#if MT32EMU_MONITOR_TIMBRES >= 2
#define DT(x) printDebug(" " #x ": %d", timbre->x)
			DT(common.partialStructure12);
			DT(common.partialStructure34);
			DT(common.partialMute);
			DT(common.noSustain);

#define DTP(x) \
			DT(partial[x].wg.pitchCoarse); \
			DT(partial[x].wg.pitchFine); \
			DT(partial[x].wg.pitchKeyfollow); \
			DT(partial[x].wg.pitchBenderEnabled); \
			DT(partial[x].wg.waveform); \
			DT(partial[x].wg.pcmWave); \
			DT(partial[x].wg.pulseWidth); \
			DT(partial[x].wg.pulseWidthVeloSensitivity); \
			DT(partial[x].pitchEnv.depth); \
			DT(partial[x].pitchEnv.veloSensitivity); \
			DT(partial[x].pitchEnv.timeKeyfollow); \
			DT(partial[x].pitchEnv.time[0]); \
			DT(partial[x].pitchEnv.time[1]); \
			DT(partial[x].pitchEnv.time[2]); \
			DT(partial[x].pitchEnv.time[3]); \
			DT(partial[x].pitchEnv.level[0]); \
			DT(partial[x].pitchEnv.level[1]); \
			DT(partial[x].pitchEnv.level[2]); \
			DT(partial[x].pitchEnv.level[3]); \
			DT(partial[x].pitchEnv.level[4]); \
			DT(partial[x].pitchLFO.rate); \
			DT(partial[x].pitchLFO.depth); \
			DT(partial[x].pitchLFO.modSensitivity); \
			DT(partial[x].tvf.cutoff); \
			DT(partial[x].tvf.resonance); \
			DT(partial[x].tvf.keyfollow); \
			DT(partial[x].tvf.biasPoint); \
			DT(partial[x].tvf.biasLevel); \
			DT(partial[x].tvf.envDepth); \
			DT(partial[x].tvf.envVeloSensitivity); \
			DT(partial[x].tvf.envDepthKeyfollow); \
			DT(partial[x].tvf.envTimeKeyfollow); \
			DT(partial[x].tvf.envTime[0]); \
			DT(partial[x].tvf.envTime[1]); \
			DT(partial[x].tvf.envTime[2]); \
			DT(partial[x].tvf.envTime[3]); \
			DT(partial[x].tvf.envTime[4]); \
			DT(partial[x].tvf.envLevel[0]); \
			DT(partial[x].tvf.envLevel[1]); \
			DT(partial[x].tvf.envLevel[2]); \
			DT(partial[x].tvf.envLevel[3]); \
			DT(partial[x].tva.level); \
			DT(partial[x].tva.veloSensitivity); \
			DT(partial[x].tva.biasPoint1); \
			DT(partial[x].tva.biasLevel1); \
			DT(partial[x].tva.biasPoint2); \
			DT(partial[x].tva.biasLevel2); \
			DT(partial[x].tva.envTimeKeyfollow); \
			DT(partial[x].tva.envTimeVeloSensitivity); \
			DT(partial[x].tva.envTime[0]); \
			DT(partial[x].tva.envTime[1]); \
			DT(partial[x].tva.envTime[2]); \
			DT(partial[x].tva.envTime[3]); \
			DT(partial[x].tva.envTime[4]); \
			DT(partial[x].tva.envLevel[0]); \
			DT(partial[x].tva.envLevel[1]); \
			DT(partial[x].tva.envLevel[2]); \
			DT(partial[x].tva.envLevel[3]);

			DTP(0);
			DTP(1);
			DTP(2);
			DTP(3);
#undef DTP
#undef DT
#endif
#endif
			// FIXME:KG: Not sure if the stuff below should be done (for rhythm and/or parts)...
			// Does the real MT-32 automatically do this?
			for (unsigned int part = 0; part < partCount; part++) {
				if (parts[part] != NULL) {
					parts[part]->refreshTimbre(i);
				}
			}
		}
		break;
	case MR_System:
		region->write(0, off, data, len);

		reportHandler->onDeviceReconfig();
		// FIXME: We haven't properly confirmed any of this behaviour
		// In particular, we tend to reset things such as reverb even if the write contained
		// the same parameters as were already set, which may be wrong.
		// On the other hand, the real thing could be resetting things even when they aren't touched
		// by the write at all.
#if MT32EMU_MONITOR_SYSEX > 0
		printDebug("WRITE-SYSTEM:");
#endif
		if (off <= SYSTEM_MASTER_TUNE_OFF && off + len > SYSTEM_MASTER_TUNE_OFF) {
			refreshSystemMasterTune();
		}
		if (off <= SYSTEM_REVERB_LEVEL_OFF && off + len > SYSTEM_REVERB_MODE_OFF) {
			refreshSystemReverbParameters();
		}
		if (off <= SYSTEM_RESERVE_SETTINGS_END_OFF && off + len > SYSTEM_RESERVE_SETTINGS_START_OFF) {
			refreshSystemReserveSettings();
		}
		if (off <= SYSTEM_CHAN_ASSIGN_END_OFF && off + len > SYSTEM_CHAN_ASSIGN_START_OFF) {
			int firstPart = off - SYSTEM_CHAN_ASSIGN_START_OFF;
			if(firstPart < 0)
				firstPart = 0;
			int lastPart = off + len - SYSTEM_CHAN_ASSIGN_START_OFF;
			if(lastPart > int(RHYTHM_PART_NUM))
				lastPart = RHYTHM_PART_NUM;
			refreshSystemChanAssign(Bit8u(firstPart), Bit8u(lastPart));
		}
		if (off <= SYSTEM_MASTER_VOL_OFF && off + len > SYSTEM_MASTER_VOL_OFF) {
			if (isD110() && !extensions.masterVolumeEnabled) {
				// D-110: this byte is a dummy (the D-20 sends 0 there); the volume is set with the panel knob.
				mt32ram.system.masterVol = 100;
			}
			refreshSystemMasterVol();
		}
		break;
	case MR_Display:
		if (isD110()) {
			// D-110: 32 characters (16x2) at 20 00 00 - 20 00 1F; any write to 20 01 00 resets the display.
			static const unsigned int D110_DISPLAY_RESET_OFF = 0x80;
			if (off + len > D110_DISPLAY_RESET_OFF) {
				memset(extensions.d110Display, ' ', 32);
				reportHandler->onDisplayReset();
				break;
			}
			for (unsigned int i = 0; i < len && off + i < 32; i++) {
				const Bit8u c = data[i];
				extensions.d110Display[off + i] = char(c >= 32 && c < 128 ? c : ' ');
			}
			extensions.d110Display[32] = 0;
			reportHandler->showLCDMessage(extensions.d110Display);
			break;
		}
		{
			char buf[SYSEX_BUFFER_SIZE];
			memcpy(&buf, &data[0], len);
			buf[len] = 0;
#if MT32EMU_MONITOR_SYSEX > 0
			printDebug("WRITE-LCD: %s", buf);
#endif
			reportHandler->showLCDMessage(buf);
		}
		break;
	case MR_Reset:
		reset();
		break;
	case MR_D110Patches:
	case MR_D20Patches:
		// Stored only: patch memory does not affect the current sound until a patch is recalled.
		region->write(first, off, data, len);
		// A patch of zeros comes from memory saved before the initial patches (d110emu kept empty patch memory then; a
		// D-20's patch names are printable), so it is the initial patch again.
		for (unsigned int i = first; i <= last && i < 128; i++) {
			const Bit8u *bytes = reinterpret_cast<const Bit8u *>(&mt32ram.d20Patches[i]);
			bool blank = true;
			for (unsigned int k = 0; k < D20_PATCH_SIZE && blank; k++) blank = bytes[k] == 0;
			if (blank) initD20Patch(mt32ram.d20Patches[i], i);
		}
		break;
	case MR_D20RhythmSetup:
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) {
			const unsigned int panOff = i * region->entrySize + offsetof(MemParams::RhythmTemp, panpot);
			if (panOff >= first * region->entrySize + off && panOff < first * region->entrySize + off + len) clearFinePan(true, i);
		}
		if (parts[8] != NULL) {
			parts[8]->refresh();
		}
		break;
	case MR_D20Patterns:
	case MR_D20PresetPatterns:
	case MR_D20RhythmTrack:
		region->write(first, off, data, len);
		break;
	case MR_PartFinePan:
	case MR_RhythmFinePan:
		region->write(first, off, data, len);
		for (unsigned int i = first; i <= last; i++) applyFinePan(region->type == MR_RhythmFinePan, i);
		break;
	case MR_D20SystemExt:
		writeD20SystemExt(off, data, len);
		break;
	case MR_WriteRequest:
		handleWriteRequest(Bit32u(region->offset(addr)), data, len);
		break;
	}
}

void Synth::refreshSystemMasterTune() {
	// 171 is ~half a semitone.
	extensions.masterTunePitchDelta = ((mt32ram.system.masterTune - 64) * 171) >> 6; // PORTABILITY NOTE: Assumes arithmetic shift.
#if MT32EMU_MONITOR_SYSEX > 0
	//FIXME:KG: This is just an educated guess.
	// The LAPC-I documentation claims a range of 427.5Hz-452.6Hz (similar to what we have here)
	// The MT-32 documentation claims a range of 432.1Hz-457.6Hz
	float masterTune = 440.0f * EXP2F((mt32ram.system.masterTune - 64.0f) / (128.0f * 12.0f));
	printDebug(" Master Tune: %f", masterTune);
#endif
}

void Synth::refreshSystemReverbParameters() {
#if MT32EMU_MONITOR_SYSEX > 0
	printDebug(" Reverb: mode=%d, time=%d, level=%d", mt32ram.system.reverbMode, mt32ram.system.reverbTime, mt32ram.system.reverbLevel);
#endif
	if (reverbOverridden) {
#if MT32EMU_MONITOR_SYSEX > 0
		printDebug(" (Reverb overridden - ignoring)");
#endif
		return;
	}
	reportHandler->onNewReverbMode(mt32ram.system.reverbMode);
	reportHandler->onNewReverbTime(mt32ram.system.reverbTime);
	reportHandler->onNewReverbLevel(mt32ram.system.reverbLevel);

	// The MT-32 family has 4 reverb modes. The D-110 has 8 types plus OFF (Small Room, Medium Room, Medium Hall,
	// Large Hall, Plate, Delay 1-3), approximated here with the 4 emulated models.
	static const Bit8u D110_REVERB_MODELS[] = {
		REVERB_MODE_ROOM, REVERB_MODE_ROOM, REVERB_MODE_HALL, REVERB_MODE_HALL,
		REVERB_MODE_PLATE, REVERB_MODE_TAP_DELAY, REVERB_MODE_TAP_DELAY, REVERB_MODE_TAP_DELAY
	};
	const Bit8u reverbMode = mt32ram.system.reverbMode;
	const bool reverbOff = isD110() ? reverbMode >= D110_REVERB_MODE_OFF : reverbMode > REVERB_MODE_TAP_DELAY;
	const bool dSeries = isD110() && dSeriesReverbEnabled && dSeriesReverbModel != NULL;
	BReverbModel *oldReverbModel = reverbModel;
	if (reverbOff || (mt32ram.system.reverbTime == 0 && mt32ram.system.reverbLevel == 0)) {
		// Setting both time and level to 0 effectively disables wet reverb output on real devices.
		// Take a shortcut in this case to reduce CPU load.
		reverbModel = NULL;
	} else if (dSeries) {
		reverbModel = dSeriesReverbModel;
	} else {
		reverbModel = reverbModels[isD110() ? D110_REVERB_MODELS[reverbMode] : reverbMode];
	}
	if (reverbModel != oldReverbModel) {
		if (extensions.preallocatedReverbMemory) {
			if (isReverbEnabled()) {
				reverbModel->mute();
			}
		} else {
			if (oldReverbModel != NULL) {
				oldReverbModel->close();
			}
			if (isReverbEnabled()) {
				reverbModel->open();
			}
		}
	}
	if (isReverbEnabled()) {
		if (reverbModel == dSeriesReverbModel) dSeriesReverbModel->setType(reverbMode);
		reverbModel->setParameters(mt32ram.system.reverbTime, mt32ram.system.reverbLevel);
	}
}

void Synth::refreshSystemReserveSettings() {
	Bit8u rset[MAX_PART_COUNT];
	memcpy(rset, mt32ram.system.reserveSettings, BASE_PART_COUNT);
	memcpy(rset + BASE_PART_COUNT, mt32ram.systemExt.reserveSettings, EXTRA_PART_COUNT);
#if MT32EMU_MONITOR_SYSEX > 0
	printDebug(" Partial reserve: 1=%02d 2=%02d 3=%02d 4=%02d 5=%02d 6=%02d 7=%02d 8=%02d Rhythm=%02d", rset[0], rset[1], rset[2], rset[3], rset[4], rset[5], rset[6], rset[7], rset[8]);
#endif
	if (partialManager != NULL) partialManager->setReserve(rset);
}

void Synth::refreshSystemChanAssign(Bit8u firstPart, Bit8u lastPart) {
	memset(extensions.chantable, 0xFF, sizeof(extensions.chantable));

	// CONFIRMED: In the case of assigning a MIDI channel to multiple parts,
	//            the messages received on that MIDI channel are handled by all the parts.
	for (Bit32u i = 0; i < partCount; i++) {
		if (parts[i] != NULL && i >= firstPart && i <= lastPart) {
			// CONFIRMED: Decay is started for all polys, and all controllers are reset, for every part whose assignment was touched by the sysex write.
			parts[i]->allSoundOff();
			parts[i]->resetAllControllers();
		}
		Bit8u chan = i < BASE_PART_COUNT ? mt32ram.system.chanAssign[i] : mt32ram.systemExt.chanAssign[i - BASE_PART_COUNT];
		if (chan > 15) continue;
		Bit8u *chanParts = extensions.chantable[chan];
		for (Bit32u j = 0; j < MAX_PART_COUNT; j++) {
			if (chanParts[j] >= MAX_PART_COUNT) {
				chanParts[j] = Bit8u(i);
				break;
			}
		}
	}

#if MT32EMU_MONITOR_SYSEX > 0
	Bit8u *rset = mt32ram.system.chanAssign;
	printDebug(" Part assign:     1=%02d 2=%02d 3=%02d 4=%02d 5=%02d 6=%02d 7=%02d 8=%02d Rhythm=%02d", rset[0], rset[1], rset[2], rset[3], rset[4], rset[5], rset[6], rset[7], rset[8]);
#endif
}

void Synth::refreshSystemMasterVol() {
#if MT32EMU_MONITOR_SYSEX > 0
	printDebug(" Master volume: %d", mt32ram.system.masterVol);
#endif
}

void Synth::refreshSystem() {
	refreshSystemMasterTune();
	refreshSystemReverbParameters();
	refreshSystemReserveSettings();
	refreshSystemChanAssign(0, Bit8u(partCount - 1));
	refreshSystemMasterVol();
}

void Synth::reset() {
	if (!opened) return;
#if MT32EMU_MONITOR_SYSEX > 0
	printDebug("RESET");
#endif
	reportHandler->onDeviceReset();
	partialManager->deactivateAll();
	mt32ram = mt32default;
	for (Bit32u i = 0; i < partCount; i++) {
		parts[i]->reset();
		if (i != RHYTHM_PART_NUM) {
			parts[i]->setProgram(controlROMData[controlROMMap->programSettings + (i < BASE_PART_COUNT ? i : i - BASE_PART_COUNT)]);
		} else {
			parts[i]->refresh();
		}
	}
	refreshSystem();
	resetMasterTunePitchDelta();
	isActive();
}

void Synth::resetMasterTunePitchDelta() {
	// This effectively resets master tune to 440.0Hz.
	// Despite that the manual claims 442.0Hz is the default setting for master tune,
	// it doesn't actually take effect upon a reset due to a bug in the reset routine.
	// CONFIRMED: This bug is present in all supported Control ROMs.
	extensions.masterTunePitchDelta = 0;
#if MT32EMU_MONITOR_SYSEX > 0
	printDebug(" Actual Master Tune reset to 440.0");
#endif
}

Bit32s Synth::getMasterTunePitchDelta() const {
	return extensions.masterTunePitchDelta + extensions.gsMasterTunePitchDelta;
}

/** Defines an interface of a class that maintains storage of variable-sized data of SysEx messages. */
class MidiEventQueue::SysexDataStorage {
public:
	static MidiEventQueue::SysexDataStorage *create(Bit32u storageBufferSize);

	virtual ~SysexDataStorage() {}
	virtual Bit8u *allocate(Bit32u sysexLength) = 0;
	virtual void reclaimUnused(const Bit8u *sysexData, Bit32u sysexLength) = 0;
	virtual void dispose(const Bit8u *sysexData, Bit32u sysexLength) = 0;
};

/** Storage space for SysEx data is allocated dynamically on demand and is disposed lazily. */
class DynamicSysexDataStorage : public MidiEventQueue::SysexDataStorage {
public:
	Bit8u *allocate(Bit32u sysexLength) {
		return new Bit8u[sysexLength];
	}

	void reclaimUnused(const Bit8u *, Bit32u) {}

	void dispose(const Bit8u *sysexData, Bit32u) {
		delete[] sysexData;
	}
};

/**
 * SysEx data is stored in a preallocated buffer, that makes this kind of storage safe
 * for use in a realtime thread. Additionally, the space retained by a SysEx event,
 * that has been processed and thus is no longer necessary, is disposed instantly.
 */
class BufferedSysexDataStorage : public MidiEventQueue::SysexDataStorage {
public:
	explicit BufferedSysexDataStorage(Bit32u useStorageBufferSize) :
		storageBuffer(new Bit8u[useStorageBufferSize]),
		storageBufferSize(useStorageBufferSize),
		startPosition(),
		endPosition()
	{}

	~BufferedSysexDataStorage() {
		delete[] storageBuffer;
	}

	Bit8u *allocate(Bit32u sysexLength) {
		Bit32u myStartPosition = startPosition;
		Bit32u myEndPosition = endPosition;

		// When the free space isn't contiguous, the data is allocated either right after the end position
		// or at the buffer beginning, wherever it fits.
		if (myStartPosition > myEndPosition) {
			if (myStartPosition - myEndPosition <= sysexLength) return NULL;
		} else if (storageBufferSize - myEndPosition < sysexLength) {
			// There's not enough free space at the end to place the data block.
			if (myStartPosition == myEndPosition) {
				// The buffer is empty -> reset positions to the buffer beginning.
				if (storageBufferSize <= sysexLength) return NULL;
				if (myStartPosition != 0) {
					myStartPosition = 0;
					// It's OK to write startPosition here non-atomically. We don't expect any
					// concurrent reads, as there must be no SysEx messages in the queue.
					startPosition = myStartPosition;
				}
			} else if (myStartPosition <= sysexLength) return NULL;
			myEndPosition = 0;
		}
		endPosition = myEndPosition + sysexLength;
		return storageBuffer + myEndPosition;
	}

	void reclaimUnused(const Bit8u *sysexData, Bit32u sysexLength) {
		if (sysexData == NULL) return;
		Bit32u allocatedPosition = startPosition;
		if (storageBuffer + allocatedPosition == sysexData) {
			startPosition = allocatedPosition + sysexLength;
		} else if (storageBuffer == sysexData) {
			// Buffer wrapped around.
			startPosition = sysexLength;
		}
	}

	void dispose(const Bit8u *, Bit32u) {}

private:
	Bit8u * const storageBuffer;
	const Bit32u storageBufferSize;

	volatile Bit32u startPosition;
	volatile Bit32u endPosition;
};

MidiEventQueue::SysexDataStorage *MidiEventQueue::SysexDataStorage::create(Bit32u storageBufferSize) {
	if (storageBufferSize > 0) {
		return new BufferedSysexDataStorage(storageBufferSize);
	} else {
		return new DynamicSysexDataStorage;
	}
}

MidiEventQueue::MidiEventQueue(Bit32u useRingBufferSize, Bit32u storageBufferSize) :
	sysexDataStorage(*SysexDataStorage::create(storageBufferSize)),
	ringBuffer(new MidiEvent[useRingBufferSize]), ringBufferMask(useRingBufferSize - 1)
{
	for (Bit32u i = 0; i <= ringBufferMask; i++) {
		ringBuffer[i].sysexData = NULL;
	}
	reset();
}

MidiEventQueue::~MidiEventQueue() {
	for (Bit32u i = 0; i <= ringBufferMask; i++) {
		volatile MidiEvent &currentEvent = ringBuffer[i];
		sysexDataStorage.dispose(currentEvent.sysexData, currentEvent.sysexLength);
	}
	delete &sysexDataStorage;
	delete[] ringBuffer;
}

void MidiEventQueue::reset() {
	startPosition = 0;
	endPosition = 0;
}

bool MidiEventQueue::pushShortMessage(Bit32u shortMessageData, Bit32u timestamp) {
	Bit32u newEndPosition = (endPosition + 1) & ringBufferMask;
	// If ring buffer is full, bail out.
	if (startPosition == newEndPosition) return false;
	volatile MidiEvent &newEvent = ringBuffer[endPosition];
	sysexDataStorage.dispose(newEvent.sysexData, newEvent.sysexLength);
	newEvent.sysexData = NULL;
	newEvent.shortMessageData = shortMessageData;
	newEvent.timestamp = timestamp;
	endPosition = newEndPosition;
	return true;
}

bool MidiEventQueue::pushSysex(const Bit8u *sysexData, Bit32u sysexLength, Bit32u timestamp) {
	Bit32u newEndPosition = (endPosition + 1) & ringBufferMask;
	// If ring buffer is full, bail out.
	if (startPosition == newEndPosition) return false;
	volatile MidiEvent &newEvent = ringBuffer[endPosition];
	sysexDataStorage.dispose(newEvent.sysexData, newEvent.sysexLength);
	Bit8u *dstSysexData = sysexDataStorage.allocate(sysexLength);
	if (dstSysexData == NULL) return false;
	memcpy(dstSysexData, sysexData, sysexLength);
	newEvent.sysexData = dstSysexData;
	newEvent.sysexLength = sysexLength;
	newEvent.timestamp = timestamp;
	endPosition = newEndPosition;
	return true;
}

const volatile MidiEventQueue::MidiEvent *MidiEventQueue::peekMidiEvent() {
	return isEmpty() ? NULL : &ringBuffer[startPosition];
}

void MidiEventQueue::dropMidiEvent() {
	if (isEmpty()) return;
	volatile MidiEvent &unusedEvent = ringBuffer[startPosition];
	sysexDataStorage.reclaimUnused(unusedEvent.sysexData, unusedEvent.sysexLength);
	startPosition = (startPosition + 1) & ringBufferMask;
}

bool MidiEventQueue::isEmpty() const {
	return startPosition == endPosition;
}

void Synth::selectRendererType(RendererType newRendererType) {
	extensions.selectedRendererType = newRendererType;
}

RendererType Synth::getSelectedRendererType() const {
	return extensions.selectedRendererType;
}

Bit32u Synth::getStereoOutputSampleRate() const {
	return (analog == NULL) ? SAMPLE_RATE : analog->getOutputSampleRate();
}

template <class Sample>
void RendererImpl<Sample>::doRender(Sample *stereoStream, Bit32u len) {
	if (!isActivated()) {
		incRenderedSampleCount(getAnalog().getDACStreamsLength(len));
		if (!getAnalog().process(NULL, NULL, NULL, NULL, NULL, NULL, stereoStream, len)) {
			printDebug("RendererImpl: Invalid call to Analog::process()!\n");
		}
		Synth::muteSampleBuffer(stereoStream, len << 1);
		return;
	}

	while (len > 0) {
		// As in AnalogOutputMode_ACCURATE mode output is upsampled, MAX_SAMPLES_PER_RUN is more than enough for the temp buffers.
		Bit32u thisPassLen = len > MAX_SAMPLES_PER_RUN ? MAX_SAMPLES_PER_RUN : len;
		doRenderStreams(tmpBuffers, getAnalog().getDACStreamsLength(thisPassLen));
		if (!getAnalog().process(stereoStream, tmpNonReverbLeft, tmpNonReverbRight, tmpReverbDryLeft, tmpReverbDryRight, tmpReverbWetLeft, tmpReverbWetRight, thisPassLen)) {
			printDebug("RendererImpl: Invalid call to Analog::process()!\n");
			Synth::muteSampleBuffer(stereoStream, len << 1);
			return;
		}
		stereoStream += thisPassLen << 1;
		len -= thisPassLen;
	}
}

template <class Sample>
template <class O>
void RendererImpl<Sample>::doRenderAndConvert(O *stereoStream, Bit32u len) {
	Sample renderingBuffer[MAX_SAMPLES_PER_RUN << 1];
	while (len > 0) {
		Bit32u thisPassLen = len > MAX_SAMPLES_PER_RUN ? MAX_SAMPLES_PER_RUN : len;
		doRender(renderingBuffer, thisPassLen);
		convertSampleFormat(renderingBuffer, stereoStream, thisPassLen << 1);
		stereoStream += thisPassLen << 1;
		len -= thisPassLen;
	}
}

template<>
void RendererImpl<IntSample>::render(IntSample *stereoStream, Bit32u len) {
	doRender(stereoStream, len);
}

template<>
void RendererImpl<IntSample>::render(FloatSample *stereoStream, Bit32u len) {
	doRenderAndConvert(stereoStream, len);
}

template<>
void RendererImpl<FloatSample>::render(IntSample *stereoStream, Bit32u len) {
	doRenderAndConvert(stereoStream, len);
}

template<>
void RendererImpl<FloatSample>::render(FloatSample *stereoStream, Bit32u len) {
	doRender(stereoStream, len);
}

// One pass (at most MAX_SAMPLES_PER_RUN frames) of the mix, as doRender() makes it, and of each part's own output into
// partPassBuffers. All the analog stages take every pass, silent or not, so that they keep in step.
template <class Sample>
void RendererImpl<Sample>::renderPass(Sample *stereoStream, Bit32u len) {
	if (!isActivated()) {
		incRenderedSampleCount(getAnalog().getDACStreamsLength(len));
		if (!getAnalog().process(static_cast<Sample *>(NULL), NULL, NULL, NULL, NULL, NULL, NULL, len)) {
			printDebug("RendererImpl: Invalid call to Analog::process()!\n");
		}
		Synth::muteSampleBuffer(stereoStream, len << 1);
		for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
			Analog *partAnalog = getPartAnalog(stream);
			if (partAnalog == NULL) continue;
			partAnalog->process(static_cast<Sample *>(NULL), NULL, NULL, NULL, NULL, NULL, NULL, len);
			Synth::muteSampleBuffer(partPassBuffers[stream], len << 1);
		}
		return;
	}
	partStreamsWanted = true;
	doRenderStreams(tmpBuffers, getAnalog().getDACStreamsLength(len));
	partStreamsWanted = false;
	if (!getAnalog().process(stereoStream, tmpNonReverbLeft, tmpNonReverbRight, tmpReverbDryLeft, tmpReverbDryRight, tmpReverbWetLeft, tmpReverbWetRight, len)) {
		printDebug("RendererImpl: Invalid call to Analog::process()!\n");
		Synth::muteSampleBuffer(stereoStream, len << 1);
	}
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
		Analog *partAnalog = getPartAnalog(stream);
		if (partAnalog == NULL) continue;
		// An output of its own is dry: its stream goes in as the mix's dry streams do, without reverb.
		if (!partAnalog->process(partPassBuffers[stream], tmpPartLeft[stream], tmpPartRight[stream], tmpSilence, tmpSilence, tmpSilence, tmpSilence, len)) {
			Synth::muteSampleBuffer(partPassBuffers[stream], len << 1);
		}
	}
}

template <class Sample>
void RendererImpl<Sample>::render(float *stereoStream, float *const *partStreams, float *const *multiStreams, Bit32u len) {
	// The output streams wanted: the parts', then the MULTI pairs'.
	float *streams[OUTPUT_STREAM_COUNT];
	for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
		if (stream < MAX_PART_COUNT) {
			streams[stream] = partStreams != NULL ? partStreams[stream] : NULL;
		} else {
			streams[stream] = multiStreams != NULL ? multiStreams[stream - MAX_PART_COUNT] : NULL;
		}
	}
	if (tmpScratchLeft == NULL) {
		// No part outputs: the mix as render() makes it, and silent streams.
		render(stereoStream, len);
		for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
			if (streams[stream] != NULL) Synth::muteSampleBuffer(streams[stream], len << 1);
		}
		return;
	}
	Sample passBuffer[MAX_SAMPLES_PER_RUN << 1];
	for (Bit32u done = 0; done < len;) {
		const Bit32u thisPassLen = len - done > MAX_SAMPLES_PER_RUN ? MAX_SAMPLES_PER_RUN : len - done;
		renderPass(passBuffer, thisPassLen);
		convertSampleFormat(passBuffer, stereoStream + (done << 1), thisPassLen << 1);
		for (unsigned int stream = 0; stream < OUTPUT_STREAM_COUNT; stream++) {
			if (streams[stream] == NULL) continue;
			if (partPassBuffers[stream] != NULL) {
				convertSampleFormat(partPassBuffers[stream], streams[stream] + (done << 1), thisPassLen << 1);
			} else {
				Synth::muteSampleBuffer(streams[stream] + (done << 1), thisPassLen << 1);  // Not built (the parts' without part streams)
			}
		}
		done += thisPassLen;
	}
}

template <class S>
static inline void renderStereo(bool opened, Renderer *renderer, S *stream, Bit32u len) {
	if (opened) {
		renderer->render(stream, len);
	} else {
		Synth::muteSampleBuffer(stream, len << 1);
	}
}

void Synth::render(Bit16s *stream, Bit32u len) {
	renderStereo(opened, renderer, stream, len);
}

void Synth::render(float *stream, Bit32u len) {
	renderStereo(opened, renderer, stream, len);
}

void Synth::render(float *stream, Bit32u len, float *const *partStreams) {
	render(stream, len, partStreams, NULL);
}

void Synth::render(float *stream, Bit32u len, float *const *partStreams, float *const *multiStreams) {
	if (opened) {
		renderer->render(stream, partStreams, multiStreams, len);
		return;
	}
	muteSampleBuffer(stream, len << 1);
	for (unsigned int part = 0; partStreams != NULL && part < MAX_PART_COUNT; part++) {
		if (partStreams[part] != NULL) muteSampleBuffer(partStreams[part], len << 1);
	}
	for (unsigned int pair = 0; multiStreams != NULL && pair < MULTI_OUTPUT_PAIRS; pair++) {
		if (multiStreams[pair] != NULL) muteSampleBuffer(multiStreams[pair], len << 1);
	}
}

template <class Sample>
static inline void advanceStream(Sample *&stream, Bit32u len) {
	if (stream != NULL) {
		stream += len;
	}
}

template <class Sample>
static inline void advanceStreams(DACOutputStreams<Sample> &streams, Bit32u len) {
	advanceStream(streams.nonReverbLeft, len);
	advanceStream(streams.nonReverbRight, len);
	advanceStream(streams.reverbDryLeft, len);
	advanceStream(streams.reverbDryRight, len);
	advanceStream(streams.reverbWetLeft, len);
	advanceStream(streams.reverbWetRight, len);
}

template <class Sample>
static inline void muteStreams(const DACOutputStreams<Sample> &streams, Bit32u len) {
	Synth::muteSampleBuffer(streams.nonReverbLeft, len);
	Synth::muteSampleBuffer(streams.nonReverbRight, len);
	Synth::muteSampleBuffer(streams.reverbDryLeft, len);
	Synth::muteSampleBuffer(streams.reverbDryRight, len);
	Synth::muteSampleBuffer(streams.reverbWetLeft, len);
	Synth::muteSampleBuffer(streams.reverbWetRight, len);
}

template <class I, class O>
static inline void convertStreamsFormat(const DACOutputStreams<I> &inStreams, const DACOutputStreams<O> &outStreams, Bit32u len) {
	convertSampleFormat(inStreams.nonReverbLeft, outStreams.nonReverbLeft, len);
	convertSampleFormat(inStreams.nonReverbRight, outStreams.nonReverbRight, len);
	convertSampleFormat(inStreams.reverbDryLeft, outStreams.reverbDryLeft, len);
	convertSampleFormat(inStreams.reverbDryRight, outStreams.reverbDryRight, len);
	convertSampleFormat(inStreams.reverbWetLeft, outStreams.reverbWetLeft, len);
	convertSampleFormat(inStreams.reverbWetRight, outStreams.reverbWetRight, len);
}

template <class Sample>
void RendererImpl<Sample>::doRenderStreams(const DACOutputStreams<Sample> &streams, Bit32u len)
{
	DACOutputStreams<Sample> tmpStreams = streams;
	partStreamPosition = 0;
	while (len > 0) {
		// We need to ensure zero-duration notes will play so add minimum 1-sample delay.
		Bit32u thisLen = 1;
		if (!isAbortingPoly()) {
			const volatile MidiEventQueue::MidiEvent *nextEvent = getMidiQueue().peekMidiEvent();
			Bit32s samplesToNextEvent = (nextEvent != NULL) ? Bit32s(nextEvent->timestamp - getRenderedSampleCount()) : MAX_SAMPLES_PER_RUN;
			if (samplesToNextEvent > 0) {
				thisLen = len > MAX_SAMPLES_PER_RUN ? MAX_SAMPLES_PER_RUN : len;
				if (thisLen > Bit32u(samplesToNextEvent)) {
					thisLen = samplesToNextEvent;
				}
			} else {
				if (nextEvent->sysexData == NULL) {
					synth.playMsgNow(nextEvent->shortMessageData);
					// If a poly is aborting we don't drop the event from the queue.
					// Instead, we'll return to it again when the abortion is done.
					if (!isAbortingPoly()) {
						getMidiQueue().dropMidiEvent();
					}
				} else {
					synth.playSysexNow(nextEvent->sysexData, nextEvent->sysexLength);
					getMidiQueue().dropMidiEvent();
				}
			}
		}
		produceStreams(tmpStreams, thisLen);
		advanceStreams(tmpStreams, thisLen);
		partStreamPosition += thisLen;
		len -= thisLen;
	}
}

template <class Sample>
template <class O>
void RendererImpl<Sample>::doRenderAndConvertStreams(const DACOutputStreams<O> &streams, Bit32u len) {
	Sample cnvNonReverbLeft[MAX_SAMPLES_PER_RUN], cnvNonReverbRight[MAX_SAMPLES_PER_RUN];
	Sample cnvReverbDryLeft[MAX_SAMPLES_PER_RUN], cnvReverbDryRight[MAX_SAMPLES_PER_RUN];
	Sample cnvReverbWetLeft[MAX_SAMPLES_PER_RUN], cnvReverbWetRight[MAX_SAMPLES_PER_RUN];

	const DACOutputStreams<Sample> cnvStreams = {
		cnvNonReverbLeft, cnvNonReverbRight,
		cnvReverbDryLeft, cnvReverbDryRight,
		cnvReverbWetLeft, cnvReverbWetRight
	};

	DACOutputStreams<O> tmpStreams = streams;

	while (len > 0) {
		Bit32u thisPassLen = len > MAX_SAMPLES_PER_RUN ? MAX_SAMPLES_PER_RUN : len;
		doRenderStreams(cnvStreams, thisPassLen);
		convertStreamsFormat(cnvStreams, tmpStreams, thisPassLen);
		advanceStreams(tmpStreams, thisPassLen);
		len -= thisPassLen;
	}
}

template<>
void RendererImpl<IntSample>::renderStreams(const DACOutputStreams<IntSample> &streams, Bit32u len) {
	doRenderStreams(streams, len);
}

template<>
void RendererImpl<IntSample>::renderStreams(const DACOutputStreams<FloatSample> &streams, Bit32u len) {
	doRenderAndConvertStreams(streams, len);
}

template<>
void RendererImpl<FloatSample>::renderStreams(const DACOutputStreams<IntSample> &streams, Bit32u len) {
	doRenderAndConvertStreams(streams, len);
}

template<>
void RendererImpl<FloatSample>::renderStreams(const DACOutputStreams<FloatSample> &streams, Bit32u len) {
	doRenderStreams(streams, len);
}

template <class S>
static inline void renderStreams(bool opened, Renderer *renderer, const DACOutputStreams<S> &streams, Bit32u len) {
	if (opened) {
		renderer->renderStreams(streams, len);
	} else {
		muteStreams(streams, len);
	}
}

void Synth::renderStreams(const DACOutputStreams<Bit16s> &streams, Bit32u len) {
	MT32Emu::renderStreams(opened, renderer, streams, len);
}

void Synth::renderStreams(const DACOutputStreams<float> &streams, Bit32u len) {
	MT32Emu::renderStreams(opened, renderer, streams, len);
}

void Synth::renderStreams(
	Bit16s *nonReverbLeft, Bit16s *nonReverbRight,
	Bit16s *reverbDryLeft, Bit16s *reverbDryRight,
	Bit16s *reverbWetLeft, Bit16s *reverbWetRight,
	Bit32u len)
{
	DACOutputStreams<IntSample> streams = {
		nonReverbLeft, nonReverbRight,
		reverbDryLeft, reverbDryRight,
		reverbWetLeft, reverbWetRight
	};
	renderStreams(streams, len);
}

void Synth::renderStreams(
	float *nonReverbLeft, float *nonReverbRight,
	float *reverbDryLeft, float *reverbDryRight,
	float *reverbWetLeft, float *reverbWetRight,
	Bit32u len)
{
	DACOutputStreams<FloatSample> streams = {
		nonReverbLeft, nonReverbRight,
		reverbDryLeft, reverbDryRight,
		reverbWetLeft, reverbWetRight
	};
	renderStreams(streams, len);
}

// In GENERATION2 units, the output from LA32 goes to the Boss chip already bit-shifted.
// In NICE mode, it's also better to increase volume before the reverb processing to preserve accuracy.
template <>
void RendererImpl<IntSample>::produceLA32Output(IntSample *buffer, Bit32u len) {
	switch (synth.getDACInputMode()) {
		case DACInputMode_GENERATION2:
			while (len--) {
				*buffer = (*buffer & 0x8000) | ((*buffer << 1) & 0x7FFE) | ((*buffer >> 14) & 0x0001);
				++buffer;
			}
			break;
		case DACInputMode_NICE:
			while (len--) {
				*buffer = Synth::clipSampleEx(IntSampleEx(*buffer) << 1);
				++buffer;
			}
			break;
		default:
			break;
	}
}

template <>
void RendererImpl<IntSample>::convertSamplesToOutput(IntSample *buffer, Bit32u len) {
	if (synth.getDACInputMode() == DACInputMode_GENERATION1) {
		while (len--) {
			*buffer = IntSample((*buffer & 0x8000) | ((*buffer << 1) & 0x7FFE));
			++buffer;
		}
	}
}

static inline float produceDistortedSample(float sample) {
	// Here we roughly simulate the distortion caused by the DAC bit shift.
	if (sample < -1.0f) {
		return sample + 2.0f;
	} else if (1.0f < sample) {
		return sample - 2.0f;
	}
	return sample;
}

template <>
void RendererImpl<FloatSample>::produceLA32Output(FloatSample *buffer, Bit32u len) {
	switch (synth.getDACInputMode()) {
	case DACInputMode_NICE:
		// Note, we do not do any clamping for floats here to avoid introducing distortions.
		// This means that the output signal may actually overshoot the unity when the volume is set too high.
		// We leave it up to the consumer whether the output is to be clamped or properly normalised further on.
		while (len--) {
			*buffer *= 2.0f;
			buffer++;
		}
		break;
	case DACInputMode_GENERATION2:
		while (len--) {
			*buffer = produceDistortedSample(2.0f * *buffer);
			buffer++;
		}
		break;
	default:
		break;
	}
}

template <>
void RendererImpl<FloatSample>::convertSamplesToOutput(FloatSample *buffer, Bit32u len) {
	if (synth.getDACInputMode() == DACInputMode_GENERATION1) {
		while (len--) {
			*buffer = produceDistortedSample(2.0f * *buffer);
			buffer++;
		}
	}
}

// Adds `src` to `dst` as partials mix into a stream.
static inline void mixSamples(IntSample *dst, const IntSample *src, Bit32u len) {
	while (len--) {
		*dst = Synth::clipSampleEx(IntSampleEx(*dst) + IntSampleEx(*src));
		dst++;
		src++;
	}
}

static inline void mixSamples(FloatSample *dst, const FloatSample *src, Bit32u len) {
	while (len--) {
		*(dst++) += *(src++);
	}
}

template <class Sample>
void RendererImpl<Sample>::produceStreams(const DACOutputStreams<Sample> &streams, Bit32u len) {
	if (isActivated()) {
		// Even if LA32 output isn't desired, we proceed anyway with temp buffers
		Sample *nonReverbLeft = streams.nonReverbLeft == NULL ? tmpNonReverbLeft : streams.nonReverbLeft;
		Sample *nonReverbRight = streams.nonReverbRight == NULL ? tmpNonReverbRight : streams.nonReverbRight;
		Sample *reverbDryLeft = streams.reverbDryLeft == NULL ? tmpReverbDryLeft : streams.reverbDryLeft;
		Sample *reverbDryRight = streams.reverbDryRight == NULL ? tmpReverbDryRight : streams.reverbDryRight;

		Synth::muteSampleBuffer(nonReverbLeft, len);
		Synth::muteSampleBuffer(nonReverbRight, len);
		Synth::muteSampleBuffer(reverbDryLeft, len);
		Synth::muteSampleBuffer(reverbDryRight, len);

		const Bit32u mutedParts = synth.getMutedParts();
		if (mutedParts != 0) {
			Synth::muteSampleBuffer(tmpMutedLeft, len);
			Synth::muteSampleBuffer(tmpMutedRight, len);
		}
		// Part outputs: each part's stream gets this stretch too, silent unless partials play out of it. (The partials of
		// a part whose output is no longer in use stay in the mix, where they began.)
		const bool partStreams = partStreamsWanted && partStreamPosition + len <= MAX_SAMPLES_PER_RUN;
		const Bit32u partOutputMask = partStreams && tmpPartLeft[0] != NULL ? synth.getPartOutputMask() : 0;
		Bit32u partsPlaying = 0;
		bool reverbSends = false;
		for (unsigned int stream = 0; partStreams && stream < OUTPUT_STREAM_COUNT; stream++) {
			if (tmpPartLeft[stream] == NULL) continue;
			Synth::muteSampleBuffer(tmpPartLeft[stream] + partStreamPosition, len);
			Synth::muteSampleBuffer(tmpPartRight[stream] + partStreamPosition, len);
		}
		const bool multiOutputs = partStreams && synth.areMultiOutputsEnabled();
		for (unsigned int i = 0; i < synth.getPartialCount(); i++) {
			const int owner = mutedParts != 0 || partOutputMask != 0 || multiOutputs ? getPartialManager().getPartial(i)->getOwnerPart() : -1;
			const int multi = multiOutputs && owner >= 0 ? getPartialManager().getPartial(i)->getMultiOutput() : 0;
			if (owner >= 0 && (mutedParts >> owner) & 1) {
				getPartialManager().produceOutput(i, tmpMutedLeft, tmpMutedRight, len);
			} else if (owner >= 0 && ((partOutputMask >> owner) & 1) != 0 && getPartialManager().getPartial(i)->isOwnOutput()) {
				Sample *partLeft = tmpPartLeft[owner] + partStreamPosition;
				Sample *partRight = tmpPartRight[owner] + partStreamPosition;
				if (getPartialManager().shouldReverb(i)) {
					// Its sound out of the part's own output, and into the reverb as from the mix.
					if (!reverbSends) {
						Synth::muteSampleBuffer(tmpReverbSendLeft, len);
						Synth::muteSampleBuffer(tmpReverbSendRight, len);
						reverbSends = true;
					}
					Synth::muteSampleBuffer(tmpScratchLeft, len);
					Synth::muteSampleBuffer(tmpScratchRight, len);
					if (getPartialManager().produceOutput(i, tmpScratchLeft, tmpScratchRight, len)) {
						mixSamples(partLeft, tmpScratchLeft, len);
						mixSamples(partRight, tmpScratchRight, len);
						mixSamples(tmpReverbSendLeft, tmpScratchLeft, len);
						mixSamples(tmpReverbSendRight, tmpScratchRight, len);
					}
				} else {
					getPartialManager().produceOutput(i, partLeft, partRight, len);
				}
				partsPlaying |= 1u << owner;
			} else if (multi > 0) {
				const unsigned int stream = MAX_PART_COUNT + unsigned(multi - 1) / 2;
				if (getPartialManager().getPartial(i)->isMultiStereo()) {
					// A stereo pair (setMultiPairsStereo): both its channels, with the partial's pan.
					getPartialManager().produceOutput(i, tmpPartLeft[stream] + partStreamPosition, tmpPartRight[stream] + partStreamPosition, len);
				} else {
					// A MULTI output is mono, as the unit's jack: the centred partial's left and right, at its full level.
					Sample *channel = ((multi - 1) & 1) == 0 ? tmpPartLeft[stream] + partStreamPosition : tmpPartRight[stream] + partStreamPosition;
					Synth::muteSampleBuffer(tmpScratchLeft, len);
					Synth::muteSampleBuffer(tmpScratchRight, len);
					if (getPartialManager().produceOutput(i, tmpScratchLeft, tmpScratchRight, len)) {
						mixSamples(channel, tmpScratchLeft, len);
						mixSamples(channel, tmpScratchRight, len);
					}
				}
				partsPlaying |= 1u << stream;
			} else if (getPartialManager().shouldReverb(i)) {
				getPartialManager().produceOutput(i, reverbDryLeft, reverbDryRight, len);
			} else {
				getPartialManager().produceOutput(i, nonReverbLeft, nonReverbRight, len);
			}
		}

		// The reverb takes the mix's reverb partials, and with part outputs those that play out of their own outputs.
		Sample *reverbInputLeft = reverbDryLeft;
		Sample *reverbInputRight = reverbDryRight;
		if (reverbSends) {
			memcpy(tmpReverbInputLeft, reverbDryLeft, len * sizeof(Sample));
			memcpy(tmpReverbInputRight, reverbDryRight, len * sizeof(Sample));
			mixSamples(tmpReverbInputLeft, tmpReverbSendLeft, len);
			mixSamples(tmpReverbInputRight, tmpReverbSendRight, len);
			produceLA32Output(tmpReverbInputLeft, len);
			produceLA32Output(tmpReverbInputRight, len);
			reverbInputLeft = tmpReverbInputLeft;
			reverbInputRight = tmpReverbInputRight;
		}
		produceLA32Output(reverbDryLeft, len);
		produceLA32Output(reverbDryRight, len);

		if (synth.isReverbEnabled()) {
			if (!getReverbModel().process(reverbInputLeft, reverbInputRight, streams.reverbWetLeft, streams.reverbWetRight, len)) {
				printDebug("RendererImpl: Invalid call to BReverbModel::process()!\n");
			}
			if (streams.reverbWetLeft != NULL) convertSamplesToOutput(streams.reverbWetLeft, len);
			if (streams.reverbWetRight != NULL) convertSamplesToOutput(streams.reverbWetRight, len);
		} else {
			Synth::muteSampleBuffer(streams.reverbWetLeft, len);
			Synth::muteSampleBuffer(streams.reverbWetRight, len);
		}

		// Don't bother with conversion if the output is going to be unused
		if (streams.nonReverbLeft != NULL) {
			produceLA32Output(nonReverbLeft, len);
			convertSamplesToOutput(nonReverbLeft, len);
		}
		if (streams.nonReverbRight != NULL) {
			produceLA32Output(nonReverbRight, len);
			convertSamplesToOutput(nonReverbRight, len);
		}
		if (streams.reverbDryLeft != NULL) convertSamplesToOutput(reverbDryLeft, len);
		if (streams.reverbDryRight != NULL) convertSamplesToOutput(reverbDryRight, len);
		// The outputs of their own (the parts', the MULTI pairs') as the mix's dry streams.
		for (unsigned int stream = 0; partsPlaying != 0 && stream < OUTPUT_STREAM_COUNT; stream++) {
			if (((partsPlaying >> stream) & 1) == 0) continue;
			produceLA32Output(tmpPartLeft[stream] + partStreamPosition, len);
			convertSamplesToOutput(tmpPartLeft[stream] + partStreamPosition, len);
			produceLA32Output(tmpPartRight[stream] + partStreamPosition, len);
			convertSamplesToOutput(tmpPartRight[stream] + partStreamPosition, len);
		}
	} else {
		muteStreams(streams, len);
		for (unsigned int stream = 0; partStreamsWanted && partStreamPosition + len <= MAX_SAMPLES_PER_RUN && stream < OUTPUT_STREAM_COUNT; stream++) {
			if (tmpPartLeft[stream] == NULL) continue;
			Synth::muteSampleBuffer(tmpPartLeft[stream] + partStreamPosition, len);
			Synth::muteSampleBuffer(tmpPartRight[stream] + partStreamPosition, len);
		}
	}

	getPartialManager().clearAlreadyOutputed();
	incRenderedSampleCount(len);
}

void Synth::printPartialUsage(Bit32u sampleOffset) {
	unsigned int partialUsage[MAX_PART_COUNT];
	partialManager->getPerPartPartialUsage(partialUsage);
	if (sampleOffset > 0) {
		printDebug("[+%u] Partial Usage: 1:%02d 2:%02d 3:%02d 4:%02d 5:%02d 6:%02d 7:%02d 8:%02d R: %02d  TOTAL: %02d", sampleOffset, partialUsage[0], partialUsage[1], partialUsage[2], partialUsage[3], partialUsage[4], partialUsage[5], partialUsage[6], partialUsage[7], partialUsage[8], getPartialCount() - partialManager->getFreePartialCount());
	} else {
		printDebug("Partial Usage: 1:%02d 2:%02d 3:%02d 4:%02d 5:%02d 6:%02d 7:%02d 8:%02d R: %02d  TOTAL: %02d", partialUsage[0], partialUsage[1], partialUsage[2], partialUsage[3], partialUsage[4], partialUsage[5], partialUsage[6], partialUsage[7], partialUsage[8], getPartialCount() - partialManager->getFreePartialCount());
	}
}

bool Synth::hasActivePartials() const {
	if (!opened) {
		return false;
	}
	for (unsigned int partialNum = 0; partialNum < getPartialCount(); partialNum++) {
		if (partialManager->getPartial(partialNum)->isActive()) {
			return true;
		}
	}
	return false;
}

bool Synth::isActive() {
	if (!opened) {
		return false;
	}
	if (!midiQueue->isEmpty() || hasActivePartials()) {
		return true;
	}
	if (isReverbEnabled() && reverbModel->isActive()) {
		return true;
	}
	activated = false;
	return false;
}

Bit32u Synth::getPartialCount() const {
	return partialCount;
}

void Synth::getPartStates(bool *partStates) const {
	if (!opened) {
		memset(partStates, 0, 9 * sizeof(bool));
		return;
	}
	for (int partNumber = 0; partNumber < 9; partNumber++) {
		const Part *part = parts[partNumber];
		partStates[partNumber] = part->getActiveNonReleasingPartialCount() > 0;
	}
}

Bit32u Synth::getPartStates() const {
	if (!opened) return 0;
	bool partStates[9];
	getPartStates(partStates);
	Bit32u bitSet = 0;
	for (int partNumber = 8; partNumber >= 0; partNumber--) {
		bitSet = (bitSet << 1) | (partStates[partNumber] ? 1 : 0);
	}
	return bitSet;
}

void Synth::getPartialStates(PartialState *partialStates) const {
	if (!opened) {
		memset(partialStates, 0, partialCount * sizeof(PartialState));
		return;
	}
	for (unsigned int partialNum = 0; partialNum < partialCount; partialNum++) {
		partialStates[partialNum] = getPartialState(partialManager, partialNum);
	}
}

void Synth::getPartialStates(Bit8u *partialStates) const {
	if (!opened) {
		memset(partialStates, 0, ((partialCount + 3) >> 2));
		return;
	}
	for (unsigned int quartNum = 0; (4 * quartNum) < partialCount; quartNum++) {
		Bit8u packedStates = 0;
		for (unsigned int i = 0; i < 4; i++) {
			unsigned int partialNum = (4 * quartNum) + i;
			if (partialCount <= partialNum) break;
			PartialState partialState = getPartialState(partialManager, partialNum);
			packedStates |= (partialState & 3) << (2 * i);
		}
		partialStates[quartNum] = packedStates;
	}
}

Bit32u Synth::getPlayingNotes(Bit8u partNumber, Bit8u *keys, Bit8u *velocities) const {
	Bit32u playingNotes = 0;
	if (opened && (partNumber < partCount)) {
		const Part *part = parts[partNumber];
		const Poly *poly = part->getFirstActivePoly();
		while (poly != NULL) {
			keys[playingNotes] = Bit8u(poly->getKey());
			velocities[playingNotes] = Bit8u(poly->getVelocity());
			playingNotes++;
			poly = poly->getNext();
		}
	}
	return playingNotes;
}

const char *Synth::getPatchName(Bit8u partNumber) const {
	return (!opened || partNumber >= partCount) ? NULL : parts[partNumber]->getCurrentInstr();
}

const Part *Synth::getPart(Bit8u partNum) const {
	if (partNum >= partCount) {
		return NULL;
	}
	return parts[partNum];
}

// ---- D-110 behaviour ----

bool Synth::isD110() const {
	return controlROMFeatures != NULL && controlROMFeatures->d110MemoryMap;
}

namespace {

inline Bit8u clampTo(Bit8u value, Bit8u max) {
	return value > max ? max : value;
}

} // namespace

// Copies a patch memory entry into the system area and the parts' temporary areas.
void Synth::recallPatchNow(Bit8u patchNum) {
	if (!opened || !isD110() || patchNum >= 128) return;
	// Patches 64-127 are the card's (C-11-C-88); without a card, the internal patch of the same number.
	const bool card = patchNum >= 64 && extensions.cardInserted;
	const D110PatchParam &patch = card ? mt32ram.cardPatches[patchNum & 63] : mt32ram.d110Patches[patchNum & 63];

	memcpy(mt32ram.system.patchName, patch.name, sizeof(mt32ram.system.patchName));
	mt32ram.system.reverbMode = clampTo(patch.reverbMode, D110_REVERB_MODE_OFF);
	mt32ram.system.reverbTime = clampTo(patch.reverbTime, 7);
	mt32ram.system.reverbLevel = clampTo(patch.reverbLevel, 7);
	for (int i = 0; i < 9; i++) {
		mt32ram.system.reserveSettings[i] = clampTo(patch.reserveSettings[i], 32);
		mt32ram.system.chanAssign[i] = clampTo(patch.chanAssign[i], 16);
	}

	// Patch tone groups are a, b, i, c, r; the temporary area has a, b, i/c, r plus the card flag. Without a card,
	// card tones fall back to the internal tones of the same number.
	static const Bit8u TONE_GROUP_TO_TIMBRE_GROUP[] = {0, 1, 2, 2, 3};
	for (int i = 0; i < 8; i++) {
		const D110PatchPartParam &src = patch.parts[i];
		MemParams::PatchTemp &dst = mt32ram.patchTemp[i];
		dst.patch.timbreGroup = TONE_GROUP_TO_TIMBRE_GROUP[clampTo(src.toneGroup, 4)];
		dst.patch.timbreNum = clampTo(src.toneNumber, 63);
		dst.patch.keyShift = clampTo(src.keyShift, 48);
		dst.patch.fineTune = clampTo(src.fineTune, 100);
		dst.patch.benderRange = clampTo(src.benderRange, 24);
		dst.patch.assignMode = clampTo(src.assignMode, 3);
		dst.patch.reverbSwitch = clampTo(src.outputAssign, 7);
		if (src.toneGroup == 3 && extensions.cardInserted) {
			dst.patch.dummy = PART_CARD_TONES;
		} else {
			dst.patch.dummy = src.toneGroup < 2 && src.dummy == PART_ALT_TONES ? PART_ALT_TONES : 0;
		}
		dst.outputLevel = clampTo(src.outputLevel, 100);
		dst.panpot = clampTo(src.panpot, 14);
		clearFinePan(false, i);
		dst.dummyv[0] = clampTo(src.keyRangeLower, 127);
		dst.dummyv[1] = clampTo(src.keyRangeUpper, 127);
		if (parts[i] != NULL) {
			parts[i]->setTimbre(&mt32ram.timbres[parts[i]->getAbsTimbreNum()].timbre);
			parts[i]->refresh();
		}
	}
	mt32ram.patchTemp[8].outputLevel = clampTo(patch.rhythmOutputLevel, 100);
	if (parts[8] != NULL) {
		parts[8]->refresh();
	}

	refreshSystemReverbParameters();
	refreshSystemReserveSettings();
	refreshSystemChanAssign(0, 8);
	extensions.currentPatch = card ? patchNum : (patchNum & 63);
	reportHandler->onPatchRecalled(extensions.currentPatch);
}

// Stores the current system area and parts' temporary areas into a patch memory entry.
void Synth::storePatchNow(Bit8u patchNum) {
	if (patchNum >= 128 || (patchNum >= 64 && !extensions.cardInserted)) return;
	D110PatchParam &patch = patchNum >= 64 ? mt32ram.cardPatches[patchNum & 63] : mt32ram.d110Patches[patchNum];
	memcpy(patch.name, mt32ram.system.patchName, sizeof(patch.name));
	patch.reverbMode = mt32ram.system.reverbMode;
	patch.reverbTime = mt32ram.system.reverbTime;
	patch.reverbLevel = mt32ram.system.reverbLevel;
	memcpy(patch.reserveSettings, mt32ram.system.reserveSettings, sizeof(patch.reserveSettings));
	memcpy(patch.chanAssign, mt32ram.system.chanAssign, sizeof(patch.chanAssign));
	static const Bit8u TIMBRE_GROUP_TO_TONE_GROUP[] = {0, 1, 2, 4};
	for (int i = 0; i < 8; i++) {
		const MemParams::PatchTemp &src = mt32ram.patchTemp[i];
		D110PatchPartParam &dst = patch.parts[i];
		const bool cardTone = (src.patch.timbreGroup & 3) == 2 && src.patch.dummy == PART_CARD_TONES;
		dst.toneGroup = cardTone ? 3 : TIMBRE_GROUP_TO_TONE_GROUP[src.patch.timbreGroup & 3];
		dst.toneNumber = src.patch.timbreNum;
		dst.keyShift = src.patch.keyShift;
		dst.fineTune = src.patch.fineTune;
		dst.benderRange = src.patch.benderRange;
		dst.assignMode = src.patch.assignMode;
		dst.outputAssign = src.patch.reverbSwitch;
		dst.dummy = (src.patch.timbreGroup & 3) < 2 && src.patch.dummy == PART_ALT_TONES ? PART_ALT_TONES : 0;
		dst.outputLevel = src.outputLevel;
		dst.panpot = src.panpot;
		dst.keyRangeLower = src.dummyv[0];
		dst.keyRangeUpper = src.dummyv[1];
	}
	patch.rhythmOutputLevel = mt32ram.patchTemp[8].outputLevel;
	extensions.currentPatch = patchNum;
}

// Write request area (40 00 00): two bytes per request, the destination number and 0 = internal / 1 = card.
void Synth::handleWriteRequest(Bit32u off, const Bit8u *data, Bit32u len) {
	static const Bit32u TONE_WRITE_OFF = 0x00; // 40 00 00-0F, parts 1-8
	static const Bit32u TIMBRE_WRITE_OFF = 0x80; // 40 01 00-0F, parts 1-8
	static const Bit32u PATCH_WRITE_OFF = 0x100; // 40 02 00
	static const Bit32u D20_PATCH_WRITE_OFF = 0x180; // 40 03 00: the D-20's performance patch write (performance mode only)
	for (Bit32u i = 0; i + 1 < len; i += 2) {
		const Bit32u requestOff = off + i;
		const Bit8u number = data[i];
		Bit8u result = 0; // Function completed
		if (requestOff % 2 != 0) {
			continue;
		}
		const bool toCard = data[i + 1] != 0;
		if (toCard && !extensions.cardInserted) {
			result = 1; // Card not ready
		} else if (requestOff >= TONE_WRITE_OFF && requestOff < TONE_WRITE_OFF + 16) {
			const unsigned int partNum = (requestOff - TONE_WRITE_OFF) / 2;
			const unsigned int toneNum = number & 0x3F;
			const unsigned int absToneNum = (toCard ? CARD_TONE_BASE : 128) + toneNum;
			mt32ram.timbres[absToneNum].timbre = mt32ram.timbreTemp[partNum];
			mt32ram.patchTemp[partNum].patch.timbreGroup = 2;
			mt32ram.patchTemp[partNum].patch.timbreNum = Bit8u(toneNum);
			mt32ram.patchTemp[partNum].patch.dummy = toCard ? PART_CARD_TONES : 0;
			for (unsigned int part = 0; part < partCount; part++) {
				parts[part]->refreshTimbre(absToneNum);
			}
			parts[partNum]->refresh();
		} else if (requestOff >= TIMBRE_WRITE_OFF && requestOff < TIMBRE_WRITE_OFF + 16) {
			const unsigned int partNum = (requestOff - TIMBRE_WRITE_OFF) / 2;
			PatchParam &timbre = toCard ? mt32ram.cardTimbres[number & 0x7F] : mt32ram.patches[number & 0x7F];
			timbre = mt32ram.patchTemp[partNum].patch;
			// The timbre's memory decides between internal and card tones, not the part's flag; d and e stay.
			timbre.dummy = timbre.timbreGroup < 2 && timbre.dummy == PART_ALT_TONES ? PART_ALT_TONES : 0;
		} else if (requestOff == PATCH_WRITE_OFF) {
			storePatchNow(Bit8u((number & 0x3F) + (toCard ? 64 : 0)));
		} else if (requestOff == D20_PATCH_WRITE_OFF) {
			if (!extensions.performanceMode) {
				result = 3; // Wrong mode: the D-20 writes patches in performance mode
			} else if (toCard) {
				result = 1; // The emulated card holds no D-20 patches
			} else {
				mt32ram.d20Patches[number & 0x7F] = mt32ram.d20PatchTemp;
				extensions.currentPerformance = number & 0x7F; // The written patch is the current one, as on the unit
			}
		} else {
			continue;
		}
		reportHandler->onWriteRequestResult(result);
	}
}

// Tail of a D-20 system area (10 00 21 - 10 00 31): output levels of parts 1-8 and R, then pans of parts 1-8.
void Synth::writeD20SystemExt(Bit32u off, const Bit8u *data, Bit32u len) {
	bool touched[9] = {false};
	for (Bit32u i = 0; i < len; i++) {
		const Bit32u pos = off + i;
		if (pos < 9) {
			mt32ram.patchTemp[pos].outputLevel = clampTo(data[i], 100);
			touched[pos] = true;
		} else if (pos < SYSTEM_D20_EXT_SIZE) {
			mt32ram.patchTemp[pos - 9].panpot = clampTo(data[i], 14);
			clearFinePan(false, pos - 9);
			touched[pos - 9] = true;
		}
	}
	for (int part = 0; part < 9; part++) {
		if (touched[part] && parts[part] != NULL) {
			parts[part]->refresh();
		}
	}
}

bool Synth::isD110Mode() const {
	return opened && isD110();
}

void Synth::setMIDIExtensionsEnabled(bool enabled) {
	if (extensions.midiExtensions == enabled) return;
	extensions.midiExtensions = enabled;
	if (!enabled) setGSMasterTune(0);
	if (!opened) return;
	for (Bit32u i = 0; i < partCount; i++) {
		// Off, what the extensions set per channel goes; the bend range becomes the timbre's (on, the channel's).
		if (!enabled) parts[i]->resetExtensionState();
		parts[i]->updatePitchBenderRange();
	}
}


void Synth::setGSMasterTune(Bit32s tenthsOfCent) {
	extensions.gsMasterTune = tenthsOfCent;
	// 4096 pitch units an octave: 12000 tenths of a cent.
	extensions.gsMasterTunePitchDelta = (tenthsOfCent * 4096 + (tenthsOfCent < 0 ? -6000 : 6000)) / 12000;
}

Bit32s Synth::getGSMasterTune() const {
	return extensions.gsMasterTune;
}

bool Synth::isMIDIExtensionsEnabled() const {
	return extensions.midiExtensions;
}

void Synth::applyFinePan(bool rhythm, unsigned int index) {
	MemParams::FinePan &fine = rhythm ? mt32ram.rhythmFinePan[index] : mt32ram.partFinePan[index];
	unsigned int value = fine.msb * 128u + fine.lsb;
	if (value > 129) {
		value = 129;
		fine.msb = 1;
		fine.lsb = 1;
	}
	if (value == 0) return; // Follows the panpot
	// The panpot (0-14) takes the nearest step, so that patches, dumps and the display keep a close pan.
	const int pan = int(value) - 65;
	const Bit8u panpot = Bit8u(7 + (pan >= 0 ? (pan * 7 + 32) / 64 : -((-pan * 7 + 32) / 64)));
	if (rhythm) {
		mt32ram.rhythmTemp[index].panpot = panpot;
	} else {
		mt32ram.patchTemp[index].panpot = panpot;
	}
}

void Synth::clearFinePan(bool rhythm, unsigned int index) {
	MemParams::FinePan &fine = rhythm ? mt32ram.rhythmFinePan[index] : mt32ram.partFinePan[index];
	fine.msb = 0;
	fine.lsb = 0;
}

unsigned int Synth::getFinePan(unsigned int partNum, const MemParams::RhythmTemp *rhythmTemp) const {
	if (!isD110()) return 0;
	const MemParams::FinePan &fine = rhythmTemp != NULL ? mt32ram.rhythmFinePan[rhythmTemp - mt32ram.rhythmTemp] : mt32ram.partFinePan[partNum];
	const unsigned int value = fine.msb * 128u + fine.lsb;
	return value <= 129 ? value : 129;
}

void Synth::setFinePanFromMIDI(unsigned int partNum, unsigned int midiPan) {
	if (partNum >= MAX_PART_COUNT) return;
	// 0 = hard left, 64 = centre, 127 = hard right (-64..+64)
	const int pan = midiPan <= 64 ? int(midiPan) - 64 : (int(midiPan - 64) * 64 + 31) / 63;
	const unsigned int value = unsigned(pan + 65);
	mt32ram.partFinePan[partNum].msb = Bit8u(value >> 7);
	mt32ram.partFinePan[partNum].lsb = Bit8u(value & 0x7F);
}

void Synth::setMasterVolumeEnabled(bool enabled) {
	extensions.masterVolumeEnabled = enabled;
	if (!enabled && opened && isD110()) {
		mt32ram.system.masterVol = 100;
		refreshSystemMasterVol();
	}
}

bool Synth::isMasterVolumeEnabled() const {
	return extensions.masterVolumeEnabled;
}

bool Synth::setPresetTimbre(Bit32u timbreNumber, const Bit8u *data) {
	if (!opened || !isD110() || timbreNumber >= 256 || (timbreNumber >= 128 && timbreNumber < 192)) return false;
	timbresMemoryRegion->write(timbreNumber, 0, data, sizeof(TimbreParam), true);
	for (unsigned int part = 0; part < partCount; part++) {
		if (parts[part] != NULL) parts[part]->refreshTimbre(timbreNumber);
	}
	return true;
}

bool Synth::restorePresetTimbres() {
	if (!opened || !isD110()) return false;
	const bool restored = initTimbres(controlROMMap->timbreAMap, controlROMMap->timbreAOffset, 0x40, 0, controlROMMap->timbreACompressed) &&
		initTimbres(controlROMMap->timbreBMap, controlROMMap->timbreBOffset, 0x40, 64, controlROMMap->timbreBCompressed) &&
		initTimbres(controlROMMap->timbreRMap, 0, controlROMMap->timbreRCount, 192, true);
	for (unsigned int timbre = 0; timbre < 256; timbre++) {
		if (timbre >= 128 && timbre < 192) continue;
		for (unsigned int part = 0; part < partCount; part++) {
			if (parts[part] != NULL) parts[part]->refreshTimbre(timbre);
		}
	}
	return restored;
}

void Synth::resetMIDIChannels() {
	setGSMasterTune(0);
	if (!opened) return;
	for (Bit32u i = 0; i < partCount; i++) {
		parts[i]->resetMIDIState();
	}
}

void Synth::resetPartLevelsAndPans() {
	if (!isD110Mode()) return;
	// Written as a DT1 writes them, so the fine pans go and the parts take the new values.
	static const Bit8u levelAndPan[2] = {100, 7};
	for (Bit32u i = 0; i < partCount; i++) {
		writeSysexGlobal(partTempAddress(i) + Bit32u(offsetof(MemParams::PatchTemp, outputLevel)), levelAndPan, 2);
	}
	if (extensions.performanceMode) applyPerformanceNow();
}

void Synth::setSixteenPartMode(bool enabled) {
	extensions.sixteenPartMode = enabled;
}

void Synth::setCardInserted(bool inserted) {
	extensions.cardInserted = inserted && isD110();
}

bool Synth::isCardInserted() const {
	return extensions.cardInserted;
}

void Synth::selectPartTimbre(Bit8u partNumber, Bit16u timbreNumber) {
	if (!opened || partNumber >= partCount || partNumber == RHYTHM_PART_NUM) return;
	if (timbreNumber >= 256 && timbreNumber < 384 && isD110()) {
		parts[partNumber]->setAltProgram(timbreNumber & 0x7F);
	} else if (timbreNumber >= 128 && timbreNumber < 256 && extensions.cardInserted) {
		parts[partNumber]->setCardProgram(timbreNumber & 0x7F);
	} else {
		parts[partNumber]->setProgram(timbreNumber & 0x7F);
	}
}

bool Synth::isPartProgramFromCard(Bit8u partNumber) const {
	return opened && partNumber < partCount && partNumber != RHYTHM_PART_NUM && parts[partNumber]->isLastProgramCard();
}

bool Synth::isPartProgramFromAlt(Bit8u partNumber) const {
	return opened && partNumber < partCount && partNumber != RHYTHM_PART_NUM && parts[partNumber]->isLastProgramAlt();
}

bool Synth::setAltTimbre(Bit32u toneNumber, const Bit8u *data) {
	if (!opened || !isD110() || toneNumber >= TIMBRE_COUNT - ALT_TONE_BASE) return false;
	timbresMemoryRegion->write(ALT_TONE_BASE + toneNumber, 0, data, sizeof(TimbreParam), true);
	for (unsigned int part = 0; part < partCount; part++) {
		if (parts[part] != NULL) parts[part]->refreshTimbre(ALT_TONE_BASE + toneNumber);
	}
	return true;
}

void Synth::setAltTimbresEnabled(bool enabled) {
	extensions.altTimbres = enabled && isD110();
}

bool Synth::hasAltTimbres() const {
	return extensions.altTimbres;
}

bool Synth::readTone(Bit16u absToneNumber, Bit8u *data) const {
	if (!opened || absToneNumber >= TIMBRE_COUNT) return false;
	memcpy(data, &mt32ram.timbres[absToneNumber].timbre, sizeof(TimbreParam));
	return true;
}

void Synth::setPerformanceMode(bool enabled, Bit8u channel) {
	extensions.performanceChannel = channel & 0x0F;
	extensions.performanceMode = enabled && isD110Mode();
	if (!extensions.performanceMode) return;
	// Only the upper and lower tones (parts 1 and 2) play; the rhythm part keeps its channel.
	for (Bit32u i = 2; i < partCount; i++) {
		if (i == RHYTHM_PART_NUM) continue;
		if (i < BASE_PART_COUNT) {
			mt32ram.system.chanAssign[i] = 16;
		} else {
			mt32ram.systemExt.chanAssign[i - BASE_PART_COUNT] = 16;
		}
	}
	refreshSystemChanAssign(2, Bit8u(partCount - 1));
	applyPerformanceNow();
}

bool Synth::isPerformanceMode() const {
	return extensions.performanceMode;
}

Bit8u Synth::getPerformanceChannel() const {
	return extensions.performanceChannel;
}

void Synth::recallPerformance(Bit8u patchNum) {
	if (!opened || !isD110()) return;
	recallPerformanceNow(patchNum);
}

Bit8u Synth::getCurrentPerformance() const {
	return extensions.currentPerformance;
}

void Synth::setCurrentPerformance(Bit8u patchNum) {
	extensions.currentPerformance = patchNum & 0x7F;
}

void Synth::recallPerformanceNow(Bit8u patchNum) {
	patchNum &= 0x7F;
	mt32ram.d20PatchTemp = mt32ram.d20Patches[patchNum];
	extensions.currentPerformance = patchNum;
	if (extensions.performanceMode) applyPerformanceNow();
}

void Synth::applyPerformanceNow() {
	const D20PatchParam &patch = mt32ram.d20PatchTemp;
	const Bit8u keyMode = clampTo(patch.keyMode, 2);
	const unsigned int splitKey = 36 + clampTo(patch.splitPoint, 61);
	const unsigned int level = clampTo(patch.patchLevel, 100);
	const unsigned int balance = clampTo(patch.balance, 100);
	for (unsigned int t = 0; t < 2; t++) {
		const bool upper = t == 0;
		MemParams::PatchTemp &dst = mt32ram.patchTemp[t];
		dst.patch.timbreGroup = clampTo(upper ? patch.upperToneGroup : patch.lowerToneGroup, 3);
		dst.patch.timbreNum = clampTo(upper ? patch.upperToneNumber : patch.lowerToneNumber, 63);
		dst.patch.keyShift = clampTo(upper ? patch.upperKeyShift : patch.lowerKeyShift, 48);
		dst.patch.fineTune = clampTo(upper ? patch.upperFineTune : patch.lowerFineTune, 100);
		dst.patch.benderRange = clampTo(upper ? patch.upperBenderRange : patch.lowerBenderRange, 24);
		dst.patch.assignMode = clampTo(upper ? patch.upperAssignMode : patch.lowerAssignMode, 3);
		dst.patch.reverbSwitch = clampTo(upper ? patch.upperReverbSwitch : patch.lowerReverbSwitch, 1); // Mix or Mix + reverb
		dst.patch.dummy = 0;
		// U/L balance: at 50 both tones play at the patch level; towards 0 the upper tone fades, towards 100 the lower.
		const unsigned int share = upper ? balance : 100 - balance;
		dst.outputLevel = Bit8u(level * (share >= 50 ? 100 : share * 2) / 100);
		dst.panpot = 7;
		clearFinePan(false, t);
		// Whole: the upper tone over the whole keyboard; dual: both; split: the lower tone below the split point.
		Bit8u lowKey = 0, highKey = 127;
		if (keyMode == 0 && !upper) {
			lowKey = 127;
			highKey = 0; // No key: the lower tone is silent
		} else if (keyMode == 2) {
			if (upper) {
				lowKey = Bit8u(splitKey);
			} else {
				highKey = Bit8u(splitKey - 1);
			}
		}
		dst.dummyv[0] = lowKey;
		dst.dummyv[1] = highKey;
		mt32ram.system.chanAssign[t] = extensions.performanceChannel;
		if (parts[t] != NULL) {
			parts[t]->setTimbre(&mt32ram.timbres[parts[t]->getAbsTimbreNum()].timbre);
			parts[t]->refresh();
		}
	}
	mt32ram.system.reverbMode = clampTo(patch.reverbMode, D110_REVERB_MODE_OFF);
	mt32ram.system.reverbTime = clampTo(patch.reverbTime, 7);
	mt32ram.system.reverbLevel = clampTo(patch.reverbLevel, 7);
	refreshSystemReverbParameters();
	refreshSystemChanAssign(0, 1);
}

Bit32u Synth::getPartCount() const {
	return partCount;
}

bool Synth::isPartActive(Bit8u partNumber) const {
	return opened && partNumber < partCount && parts[partNumber]->getActiveNonReleasingPartialCount() > 0;
}

void Synth::refreshPartTemp(unsigned int partNum, bool timbreTouched) {
	if (partNum >= partCount || parts[partNum] == NULL) return;
	if (partNum != RHYTHM_PART_NUM && timbreTouched) {
		parts[partNum]->setTimbre(&mt32ram.timbres[parts[partNum]->getAbsTimbreNum()].timbre);
	}
	parts[partNum]->refresh();
}

Bit32u Synth::partTempAddress(unsigned int partNum) const {
	if (partNum < BASE_PART_COUNT) return MT32EMU_MEMADDR(0x030000) + partNum * Bit32u(sizeof(MemParams::PatchTemp));
	return MT32EMU_MEMADDR(0x130000) + (partNum - BASE_PART_COUNT) * Bit32u(sizeof(MemParams::PatchTemp));
}

Bit32u Synth::partToneTempAddress(unsigned int partNum) const {
	if (partNum < RHYTHM_PART_NUM) return MT32EMU_MEMADDR(0x040000) + partNum * Bit32u(sizeof(TimbreParam));
	return MT32EMU_MEMADDR(0x140000) + (partNum - BASE_PART_COUNT) * Bit32u(sizeof(TimbreParam));
}

void Synth::setControlChannel(Bit8u channel) {
	extensions.controlChannel = channel > 15 ? 16 : channel;
}

Bit8u Synth::getControlChannel() const {
	return extensions.controlChannel;
}

void Synth::setDeviceID(Bit8u deviceID) {
	extensions.deviceID = (deviceID >= 0x10 && deviceID <= 0x1F) ? deviceID : 0x10;
}

Bit8u Synth::getDeviceID() const {
	return extensions.deviceID;
}

void Synth::recallPatch(Bit8u patchNum) {
	recallPatchNow(patchNum);
}

Bit8u Synth::getCurrentPatch() const {
	return extensions.currentPatch;
}

void Synth::setCurrentPatch(Bit8u patchNum) {
	extensions.currentPatch = patchNum & 0x7F;
}

Bit8u Synth::getPartProgram(Bit8u partNumber) const {
	return (!opened || partNumber >= partCount || partNumber == RHYTHM_PART_NUM) ? 0xFF : parts[partNumber]->getLastProgram();
}

Bit8u Synth::getPartMIDIVolume(Bit8u partNumber) const {
	return (!opened || partNumber >= partCount) ? 0 : parts[partNumber]->getMIDIVolume();
}

Bit8u Synth::getPartExpression(Bit8u partNumber) const {
	return (!opened || partNumber >= partCount) ? 0 : parts[partNumber]->getExpression();
}

Bit32u Synth::getPartPitchBendRangeCents(Bit8u partNumber) const {
	return (!opened || partNumber >= partCount) ? 0 : parts[partNumber]->getPitchBendRangeCents();
}

Bit32u Synth::getPartActivePartialCount(Bit8u partNumber) const {
	if (!opened || partNumber >= partCount) return 0;
	return parts[partNumber]->getActivePartialCount();
}

Bit32u Synth::getPartReservedPartialCount(Bit8u partNumber) const {
	if (!opened || partNumber >= partCount) return 0;
	return partialManager->getReservedPartialCount(partNumber);
}

void Synth::getToneName(Bit16u absToneNumber, char *name) const {
	if (!opened || absToneNumber >= TIMBRE_COUNT) {
		name[0] = 0;
		return;
	}
	memcpy(name, mt32ram.timbres[absToneNumber].timbre.common.name, 10);
	name[10] = 0;
}

void MemoryRegion::read(unsigned int entry, unsigned int off, Bit8u *dst, unsigned int len) const {
	off += entry * entrySize;
	// This method should never be called with out-of-bounds parameters,
	// or on an unsupported region - seeing any of this debug output indicates a bug in the emulator
	if (entry >= entries || off > entrySize * entries - 1) {
#if MT32EMU_MONITOR_SYSEX > 0
		synth->printDebug("read[%d]: parameters start out of bounds: entry=%d, off=%d, len=%d", type, entry, off, len);
#endif
		return;
	}
	if (off + len > entrySize * entries) {
#if MT32EMU_MONITOR_SYSEX > 0
		synth->printDebug("read[%d]: parameters end out of bounds: entry=%d, off=%d, len=%d", type, entry, off, len);
#endif
		len = entrySize * entries - off;
	}
	Bit8u *src = getRealMemory();
	if (src == NULL) {
#if MT32EMU_MONITOR_SYSEX > 0
		synth->printDebug("read[%d]: unreadable region: entry=%d, off=%d, len=%d", type, entry, off, len);
#endif
		return;
	}
	memcpy(dst, src + off, len);
}

void MemoryRegion::write(unsigned int entry, unsigned int off, const Bit8u *src, unsigned int len, bool init) const {
	unsigned int memOff = entry * entrySize + off;
	// This method should never be called with out-of-bounds parameters,
	// or on an unsupported region - seeing any of this debug output indicates a bug in the emulator.
	// Bounds are checked on the absolute offset: upstream checked only the offset within the entry,
	// so a write past the last entry (e.g. D-20 data at 0A 00 00) overran the timbre array.
	if (entry >= entries || memOff > entrySize * entries - 1) {
#if MT32EMU_MONITOR_SYSEX > 0
		synth->printDebug("write[%d]: parameters start out of bounds: entry=%d, off=%d, len=%d", type, entry, off, len);
#endif
		return;
	}
	if (memOff + len > entrySize * entries) {
#if MT32EMU_MONITOR_SYSEX > 0
		synth->printDebug("write[%d]: parameters end out of bounds: entry=%d, off=%d, len=%d", type, entry, off, len);
#endif
		len = entrySize * entries - memOff;
	}
	Bit8u *dest = getRealMemory();
	if (dest == NULL) {
#if MT32EMU_MONITOR_SYSEX > 0
		synth->printDebug("write[%d]: unwritable region: entry=%d, off=%d, len=%d", type, entry, off, len);
#endif
		return;
	}

	for (unsigned int i = 0; i < len; i++) {
		Bit8u desiredValue = src[i];
		Bit8u maxValue = getMaxValue(memOff);
		// maxValue == 0 means write-protected unless called from initialisation code, in which case it really means the maximum value is 0.
		if (maxValue != 0 || init) {
			if (desiredValue > maxValue) {
#if MT32EMU_MONITOR_SYSEX > 0
				synth->printDebug("write[%d]: Wanted 0x%02x at %d, but max 0x%02x", type, desiredValue, memOff, maxValue);
#endif
				desiredValue = maxValue;
			}
			dest[memOff] = desiredValue;
		} else if (desiredValue != 0) {
#if MT32EMU_MONITOR_SYSEX > 0
			// Only output debug info if they wanted to write non-zero, since a lot of things cause this to spit out a lot of debug info otherwise.
			synth->printDebug("write[%d]: Wanted 0x%02x at %d, but write-protected", type, desiredValue, memOff);
#endif
		}
		memOff++;
	}
}

} // namespace MT32Emu
