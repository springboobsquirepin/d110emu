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

#ifndef MT32EMU_SYNTH_H
#define MT32EMU_SYNTH_H

#include <cstdarg>
#include <cstddef>
#include <cstring>

#include "globals.h"
#include "Types.h"
#include "Enumerations.h"
#include "Structures.h"

namespace MT32Emu {

class Analog;
class BReverbModel;
class DSeriesReverbModel;
struct DSeriesReverbSettings;
class Extensions;
class MemoryRegion;
class MidiEventQueue;
class Part;
class Poly;
class Partial;
class PartialManager;
class Renderer;
class ROMImage;

class PatchTempMemoryRegion;
class RhythmTempMemoryRegion;
class TimbreTempMemoryRegion;
class PatchesMemoryRegion;
class TimbresMemoryRegion;
class SystemMemoryRegion;
class DisplayMemoryRegion;
class ResetMemoryRegion;
class D110PatchesMemoryRegion;
class D20PatchesMemoryRegion;
class D20RhythmSetupMemoryRegion;
class D20SystemExtMemoryRegion;
class WriteRequestMemoryRegion;
class ExtPatchTempMemoryRegion;
class ExtTimbreTempMemoryRegion;
class ExtSystemMemoryRegion;
class D20PatchTempMemoryRegion;
class CardTonesMemoryRegion;
class CardTimbresMemoryRegion;
class CardPatchesMemoryRegion;
class PartFinePanMemoryRegion;
class RhythmFinePanMemoryRegion;
class D20PatternsMemoryRegion;
class D20RhythmTrackMemoryRegion;

struct ControlROMFeatureSet;
struct ControlROMMap;
struct PCMWaveEntry;
struct MemParams;

const Bit8u SYSEX_MANUFACTURER_ROLAND = 0x41;

const Bit8u SYSEX_MDL_MT32 = 0x16;
const Bit8u SYSEX_MDL_D50 = 0x14;

const Bit8u SYSEX_CMD_RQ1 = 0x11; // Request data #1
const Bit8u SYSEX_CMD_DT1 = 0x12; // Data set 1
const Bit8u SYSEX_CMD_WSD = 0x40; // Want to send data
const Bit8u SYSEX_CMD_RQD = 0x41; // Request data
const Bit8u SYSEX_CMD_DAT = 0x42; // Data set
const Bit8u SYSEX_CMD_ACK = 0x43; // Acknowledge
const Bit8u SYSEX_CMD_EOD = 0x45; // End of data
const Bit8u SYSEX_CMD_ERR = 0x4E; // Communications error
const Bit8u SYSEX_CMD_RJC = 0x4F; // Rejection

// Set of multiplexed output streams appeared at the DAC entrance.
template <class T>
struct DACOutputStreams {
	T *nonReverbLeft;
	T *nonReverbRight;
	T *reverbDryLeft;
	T *reverbDryRight;
	T *reverbWetLeft;
	T *reverbWetRight;
};

// Class for the client to supply callbacks for reporting various errors and information
class MT32EMU_EXPORT ReportHandler {
public:
	virtual ~ReportHandler() {}

	// Callback for debug messages, in vprintf() format
	virtual void printDebug(const char *fmt, va_list list);
	// Callbacks for reporting errors
	virtual void onErrorControlROM() {}
	virtual void onErrorPCMROM() {}
	// Callback for reporting about displaying a new custom message on LCD
	virtual void showLCDMessage(const char *message);
	// Callback for reporting actual processing of a MIDI message
	virtual void onMIDIMessagePlayed() {}
	// Callback for reporting an overflow of the input MIDI queue.
	// Returns true if a recovery action was taken and yet another attempt to enqueue the MIDI event is desired.
	virtual bool onMIDIQueueOverflow() { return false; }
	// Callback invoked when a System Realtime MIDI message is detected at the input.
	virtual void onMIDISystemRealtime(Bit8u /* systemRealtime */) {}
	// Callbacks for reporting system events
	virtual void onDeviceReset() {}
	virtual void onDeviceReconfig() {}
	// Callbacks for reporting changes of reverb settings
	virtual void onNewReverbMode(Bit8u /* mode */) {}
	virtual void onNewReverbTime(Bit8u /* time */) {}
	virtual void onNewReverbLevel(Bit8u /* level */) {}
	// Callbacks for reporting various information
	virtual void onPolyStateChanged(Bit8u /* partNum */) {}
	virtual void onProgramChanged(Bit8u /* partNum */, const char * /* soundGroupName */, const char * /* patchName */) {}
	// D-110: a patch was recalled into the temporary area (program change on the control channel or recallPatch()).
	virtual void onPatchRecalled(Bit8u /* patchNum */) {}
	// D-110: SysEx display reset (20 01 00); the display returns to its normal reading.
	virtual void onDisplayReset() {}
	// D-110: write request result: 0 = completed, 1 = card not ready, 2 = write protected, 3 = incorrect mode.
	virtual void onWriteRequestResult(Bit8u /* result */) {}
};

class Synth {
friend class DefaultMidiStreamParser;
friend class MemoryRegion;
friend class Part;
friend class Partial;
friend class PartialManager;
friend class Poly;
friend class Renderer;
friend class RhythmPart;
friend class SamplerateAdapter;
friend class SoxrAdapter;
friend class TVA;
friend class TVF;
friend class TVP;

private:
	// **************************** Implementation fields **************************

	PatchTempMemoryRegion *patchTempMemoryRegion;
	RhythmTempMemoryRegion *rhythmTempMemoryRegion;
	TimbreTempMemoryRegion *timbreTempMemoryRegion;
	PatchesMemoryRegion *patchesMemoryRegion;
	TimbresMemoryRegion *timbresMemoryRegion;
	SystemMemoryRegion *systemMemoryRegion;
	DisplayMemoryRegion *displayMemoryRegion;
	ResetMemoryRegion *resetMemoryRegion;
	D110PatchesMemoryRegion *d110PatchesMemoryRegion;
	D20PatchesMemoryRegion *d20PatchesMemoryRegion;
	D20RhythmSetupMemoryRegion *d20RhythmSetupMemoryRegion;
	D20SystemExtMemoryRegion *d20SystemExtMemoryRegion;
	WriteRequestMemoryRegion *writeRequestMemoryRegion;
	ExtPatchTempMemoryRegion *extPatchTempMemoryRegion;
	ExtTimbreTempMemoryRegion *extTimbreTempMemoryRegion;
	ExtSystemMemoryRegion *extSystemMemoryRegion;
	D20PatchTempMemoryRegion *d20PatchTempMemoryRegion;
	CardTonesMemoryRegion *cardTonesMemoryRegion;
	CardTimbresMemoryRegion *cardTimbresMemoryRegion;
	CardPatchesMemoryRegion *cardPatchesMemoryRegion;
	PartFinePanMemoryRegion *partFinePanMemoryRegion;
	RhythmFinePanMemoryRegion *rhythmFinePanMemoryRegion;
	D20PatternsMemoryRegion *d20PatternsMemoryRegion;
	D20PatternsMemoryRegion *d20PresetPatternsMemoryRegion;
	D20RhythmTrackMemoryRegion *d20RhythmTrackMemoryRegion;

	Bit8u *paddedTimbreMaxTable;

	PCMWaveEntry *pcmWaves; // Array

	const ControlROMFeatureSet *controlROMFeatures;
	const ControlROMMap *controlROMMap;
	// Control ROMs differ in size (64 KiB for MT-32/CM-32L, 160 KiB for D-110), so the copy is heap-allocated per instance.
	Bit8u *controlROMData;
	Bit32u controlROMSize;
	Bit16s *pcmROMData;
	size_t pcmROMSize; // This is in 16-bit samples, therefore half the number of bytes in the ROM

	Bit8u soundGroupIx[128]; // For each standard timbre
	const char (*soundGroupNames)[9]; // Array

	Bit32u partialCount;
	Bit8u nukeme[16]; // FIXME: Nuke it. For binary compatibility only.

	MidiEventQueue *midiQueue;
	volatile Bit32u lastReceivedMIDIEventTimestamp;
	volatile Bit32u renderedSampleCount;

	MemParams &mt32ram, &mt32default;

	BReverbModel *reverbModels[4];
	BReverbModel *reverbModel;
	bool reverbOverridden;
	DSeriesReverbModel *dSeriesReverbModel;       // D-110 mode: types 1-8 (DSeriesReverb.h)
	DSeriesReverbSettings *dSeriesReverbSettings;  // Kept across open() and close()
	bool dSeriesReverbEnabled;

	MIDIDelayMode midiDelayMode;
	DACInputMode dacInputMode;

	float outputGain;
	float reverbOutputGain;

	bool reversedStereoEnabled;

	bool opened;
	bool activated;

	bool isDefaultReportHandler;
	ReportHandler *reportHandler;

	PartialManager *partialManager;
	// Parts 1-8 (0-7), rhythm (8) and, in 16-part mode, parts 9-15 (9-15); partCount is 9 or 16.
	Part *parts[MAX_PART_COUNT];
	Bit32u partCount;

	// When a partial needs to be aborted to free it up for use by a new Poly,
	// the controller will busy-loop waiting for the sound to finish.
	// We emulate this by delaying new MIDI events processing until abortion finishes.
	Poly *abortingPoly;

	Analog *analog;
	// Part outputs (setPartOutputsAvailable): each part's own analog stage, then the MULTI pairs', built with the mix's so
	// that they keep in step.
	Analog *partAnalogs[OUTPUT_STREAM_COUNT];
	Renderer *renderer;

	// Binary compatibility helper.
	Extensions &extensions;

	// **************************** Implementation methods **************************

	Bit32u addMIDIInterfaceDelay(Bit32u len, Bit32u timestamp);
	bool isAbortingPoly() const { return abortingPoly != NULL; }

	void writeSysexGlobal(Bit32u addr, const Bit8u *sysex, Bit32u len);
	void readSysex(Bit8u channel, const Bit8u *sysex, Bit32u len) const;
	void initMemoryRegions();
	void deleteMemoryRegions();
	MemoryRegion *findMemoryRegion(Bit32u addr);
	void writeMemoryRegion(const MemoryRegion *region, Bit32u addr, Bit32u len, const Bit8u *data);
	void readMemoryRegion(const MemoryRegion *region, Bit32u addr, Bit32u len, Bit8u *data);

	bool loadControlROM(const ROMImage &controlROMImage);
	bool loadPCMROM(const ROMImage &pcmROMImage);

	bool initPCMList(Bit16u mapAddress, Bit16u count);
	bool initTimbres(Bit16u mapAddress, Bit16u offset, Bit16u timbreCount, Bit16u startTimbre, bool compressed);
	bool initCompressedTimbre(Bit16u drumNum, const Bit8u *mem, Bit32u memLen);
	void initReverbModels(bool mt32CompatibleMode);
	void initSoundGroups(char newSoundGroupNames[][9]);

	void refreshSystemMasterTune();
	void refreshSystemReverbParameters();
	void refreshSystemReserveSettings();
	// firstPart-lastPart: part numbers (0-16) whose channel was written; they are silenced and reset.
	void refreshSystemChanAssign(Bit8u firstPart, Bit8u lastPart);
	// Re-reads a part's temporary area after a SysEx write (timbreTouched: the tone group/number may have changed).
	void refreshPartTemp(unsigned int partNum, bool timbreTouched);
	// D-20 performance mode: copies performance patch memory into the patch temporary area, and applies that
	// to parts 1 (upper) and 2 (lower).
	void recallPerformanceNow(Bit8u patchNum);
	void applyPerformanceNow();
	// Addresses (as MT32EMU_MEMADDR) of a part's temporary areas, for channel-addressed SysEx.
	Bit32u partTempAddress(unsigned int partNum) const;
	Bit32u partToneTempAddress(unsigned int partNum) const;
	void refreshSystemMasterVol();
	void refreshSystem();
	void reset();
	void dispose();

	// D-110 behaviour
	bool isD110() const;
	void recallPatchNow(Bit8u patchNum);
	void storePatchNow(Bit8u patchNum);
	void handleWriteRequest(Bit32u off, const Bit8u *data, Bit32u len);
	void writeD20SystemExt(Bit32u off, const Bit8u *data, Bit32u len);
	// Fine pan (MemParams::FinePan): a new value moves the panpot to its nearest step; a panpot write drops it.
	void applyFinePan(bool rhythm, unsigned int index);
	void clearFinePan(bool rhythm, unsigned int index);
	// Fine pan value of a part or rhythm key: 0 when it follows the panpot, else 1-129 (-64..+64).
	unsigned int getFinePan(unsigned int partNum, const MemParams::RhythmTemp *rhythmTemp) const;
	// Sets a part's fine pan from MIDI pan (CC 10, 0-127; 64 = centre).
	void setFinePanFromMIDI(unsigned int partNum, unsigned int midiPan);

	void printPartialUsage(Bit32u sampleOffset = 0);

	void newTimbreSet(Bit8u partNum, Bit8u timbreGroup, Bit8u timbreNumber, const char patchName[]);
	void printDebug(const char *fmt, ...);

	// partNum should be 0..7 for Part 1..8, or 8 for Rhythm
	const Part *getPart(Bit8u partNum) const;

	void resetMasterTunePitchDelta();
	// The system area's master tune plus the GS master tune (MIDI extensions), in pitch units (4096 an octave).
	Bit32s getMasterTunePitchDelta() const;
	void setGSMasterTune(Bit32s tenthsOfCent);

public:
	static inline Bit16s clipSampleEx(Bit32s sampleEx) {
		// Clamp values above 32767 to 32767, and values below -32768 to -32768
		// FIXME: Do we really need this stuff? I think these branches are very well predicted. Instead, this introduces a chain.
		// The version below is actually a bit faster on my system...
		//return ((sampleEx + 0x8000) & ~0xFFFF) ? Bit16s((sampleEx >> 31) ^ 0x7FFF) : (Bit16s)sampleEx;
		return ((-0x8000 <= sampleEx) && (sampleEx <= 0x7FFF)) ? Bit16s(sampleEx) : Bit16s((sampleEx >> 31) ^ 0x7FFF);
	}

	static inline float clipSampleEx(float sampleEx) {
		return sampleEx;
	}

	template <class S>
	static inline void muteSampleBuffer(S *buffer, Bit32u len) {
		if (buffer == NULL) return;
		memset(buffer, 0, len * sizeof(S));
	}

	static inline void muteSampleBuffer(float *buffer, Bit32u len) {
		if (buffer == NULL) return;
		// FIXME: Use memset() where compatibility is guaranteed (if this turns out to be a win)
		while (len--) {
			*(buffer++) = 0.0f;
		}
	}

	static inline Bit16s convertSample(float sample) {
		return Synth::clipSampleEx(Bit32s(sample * 32768.0f)); // This multiplier corresponds to normalised floats
	}

	static inline float convertSample(Bit16s sample) {
		return float(sample) / 32768.0f; // This multiplier corresponds to normalised floats
	}

	// Returns library version as an integer in format: 0x00MMmmpp, where:
	// MM - major version number
	// mm - minor version number
	// pp - patch number
	MT32EMU_EXPORT static Bit32u getLibraryVersionInt();
	// Returns library version as a C-string in format: "MAJOR.MINOR.PATCH"
	MT32EMU_EXPORT static const char *getLibraryVersionString();

	MT32EMU_EXPORT static Bit32u getShortMessageLength(Bit32u msg);
	MT32EMU_EXPORT static Bit8u calcSysexChecksum(const Bit8u *data, const Bit32u len, const Bit8u initChecksum = 0);

	// Returns output sample rate used in emulation of stereo analog circuitry of hardware units.
	// See comment for AnalogOutputMode.
	MT32EMU_EXPORT static Bit32u getStereoOutputSampleRate(AnalogOutputMode analogOutputMode);

	// Optionally sets callbacks for reporting various errors, information and debug messages
	MT32EMU_EXPORT explicit Synth(ReportHandler *useReportHandler = NULL);
	MT32EMU_EXPORT ~Synth();

	// Used to initialise the MT-32. Must be called before any other function.
	// Returns true if initialization was sucessful, otherwise returns false.
	// controlROMImage and pcmROMImage represent Control and PCM ROM images for use by synth.
	// usePartialCount sets the maximum number of partials playing simultaneously for this session (optional).
	// analogOutputMode sets the mode for emulation of analogue circuitry of the hardware units (optional).
	MT32EMU_EXPORT bool open(const ROMImage &controlROMImage, const ROMImage &pcmROMImage, Bit32u usePartialCount = DEFAULT_MAX_PARTIALS, AnalogOutputMode analogOutputMode = AnalogOutputMode_COARSE);

	// Overloaded method which opens the synth with default partial count.
	MT32EMU_EXPORT bool open(const ROMImage &controlROMImage, const ROMImage &pcmROMImage, AnalogOutputMode analogOutputMode);

	// Closes the MT-32 and deallocates any memory used by the synthesizer
	MT32EMU_EXPORT void close();

	// Returns true if the synth is in completely initialized state, otherwise returns false.
	MT32EMU_EXPORT bool isOpen() const;

	// All the enqueued events are processed by the synth immediately.
	MT32EMU_EXPORT void flushMIDIQueue();

	// Sets size of the internal MIDI event queue. The queue size is set to the minimum power of 2 that is greater or equal to the size specified.
	// The queue is flushed before reallocation.
	// Returns the actual queue size being used.
	MT32EMU_EXPORT Bit32u setMIDIEventQueueSize(Bit32u requestedSize);

	// Configures the SysEx storage of the internal MIDI event queue.
	// Supplying 0 in the storageBufferSize argument makes the SysEx data stored
	// in multiple dynamically allocated buffers per MIDI event. These buffers are only disposed
	// when a new MIDI event replaces the SysEx event in the queue, thus never on the rendering thread.
	// This is the default behaviour.
	// In contrast, when a positive value is specified, SysEx data will be stored in a single preallocated buffer,
	// which makes this kind of storage safe for use in a realtime thread. Additionally, the space retained
	// by a SysEx event, that has been processed and thus is no longer necessary, is disposed instantly.
	// Note, the queue is flushed and recreated in the process so that its size remains intact.
	MT32EMU_EXPORT void configureMIDIEventQueueSysexStorage(Bit32u storageBufferSize);

	// Returns current value of the global counter of samples rendered since the synth was created (at the native sample rate 32000 Hz).
	// This method helps to compute accurate timestamp of a MIDI message to use with the methods below.
	MT32EMU_EXPORT Bit32u getInternalRenderedSampleCount() const;

	// Enqueues a MIDI event for subsequent playback.
	// The MIDI event will be processed not before the specified timestamp.
	// The timestamp is measured as the global rendered sample count since the synth was created (at the native sample rate 32000 Hz).
	// The minimum delay involves emulation of the delay introduced while the event is transferred via MIDI interface
	// and emulation of the MCU busy-loop while it frees partials for use by a new Poly.
	// Calls from multiple threads must be synchronised, although, no synchronisation is required with the rendering thread.
	// The methods return false if the MIDI event queue is full and the message cannot be enqueued.

	// Enqueues a single short MIDI message to play at specified time. The message must contain a status byte.
	MT32EMU_EXPORT bool playMsg(Bit32u msg, Bit32u timestamp);
	// Enqueues a single well formed System Exclusive MIDI message to play at specified time.
	MT32EMU_EXPORT bool playSysex(const Bit8u *sysex, Bit32u len, Bit32u timestamp);

	// Enqueues a single short MIDI message to be processed ASAP. The message must contain a status byte.
	MT32EMU_EXPORT bool playMsg(Bit32u msg);
	// Enqueues a single well formed System Exclusive MIDI message to be processed ASAP.
	MT32EMU_EXPORT bool playSysex(const Bit8u *sysex, Bit32u len);

	// WARNING:
	// The methods below don't ensure minimum 1-sample delay between sequential MIDI events,
	// and a sequence of NoteOn and immediately succeeding NoteOff messages is always silent.
	// A thread that invokes these methods must be explicitly synchronised with the thread performing sample rendering.

	// Sends a short MIDI message to the synth for immediate playback. The message must contain a status byte.
	// See the WARNING above.
	MT32EMU_EXPORT void playMsgNow(Bit32u msg);
	// Sends unpacked short MIDI message to the synth for immediate playback. The message must contain a status byte.
	// See the WARNING above.
	MT32EMU_EXPORT void playMsgOnPart(Bit8u part, Bit8u code, Bit8u note, Bit8u velocity);

	// Sends a single well formed System Exclusive MIDI message for immediate processing. The length is in bytes.
	// See the WARNING above.
	MT32EMU_EXPORT void playSysexNow(const Bit8u *sysex, Bit32u len);
	// Sends inner body of a System Exclusive MIDI message for direct processing. The length is in bytes.
	// See the WARNING above.
	MT32EMU_EXPORT void playSysexWithoutFraming(const Bit8u *sysex, Bit32u len);
	// Sends inner body of a System Exclusive MIDI message for direct processing. The length is in bytes.
	// See the WARNING above.
	MT32EMU_EXPORT void playSysexWithoutHeader(Bit8u device, Bit8u command, const Bit8u *sysex, Bit32u len);
	// Sends inner body of a System Exclusive MIDI message for direct processing. The length is in bytes.
	// See the WARNING above.
	MT32EMU_EXPORT void writeSysex(Bit8u channel, const Bit8u *sysex, Bit32u len);

	// Allows to disable wet reverb output altogether.
	MT32EMU_EXPORT void setReverbEnabled(bool reverbEnabled);
	// Returns whether wet reverb output is enabled.
	MT32EMU_EXPORT bool isReverbEnabled() const;
	// Sets override reverb mode. In this mode, emulation ignores sysexes (or the related part of them) which control the reverb parameters.
	// This mode is in effect until it is turned off. When the synth is re-opened, the override mode is unchanged but the state
	// of the reverb model is reset to default.
	MT32EMU_EXPORT void setReverbOverridden(bool reverbOverridden);
	// Returns whether reverb settings are overridden.
	MT32EMU_EXPORT bool isReverbOverridden() const;
	// Forces reverb model compatibility mode. By default, the compatibility mode corresponds to the used control ROM version.
	// Invoking this method with the argument set to true forces emulation of old MT-32 reverb circuit.
	// When the argument is false, emulation of the reverb circuit used in new generation of MT-32 compatible modules is enforced
	// (these include CM-32L and LAPC-I).
	MT32EMU_EXPORT void setReverbCompatibilityMode(bool mt32CompatibleMode);
	// Returns whether reverb is in old MT-32 compatibility mode.
	MT32EMU_EXPORT bool isMT32ReverbCompatibilityMode() const;
	// Returns whether default reverb compatibility mode is the old MT-32 compatibility mode.
	MT32EMU_EXPORT bool isDefaultReverbMT32Compatible() const;
	// If enabled, reverb buffers for all modes are keept around allocated all the time to avoid memory
	// allocating/freeing in the rendering thread, which may be required for realtime operation.
	// Otherwise, reverb buffers that are not in use are deleted to save memory (the default behaviour).
	MT32EMU_EXPORT void preallocateReverbMemory(bool enabled);
	// D-110 mode: reverb types 1-8 play through the D-series reverb model (DSeriesReverb.h), the default, or when
	// disabled through the MT-32 family's four models as before (Small/Medium Room -> Room, the halls -> Hall, Plate,
	// Delay 1-3 -> Tap delay).
	MT32EMU_EXPORT void setDSeriesReverbEnabled(bool enabled);
	MT32EMU_EXPORT bool isDSeriesReverbEnabled() const;
	// The D-series reverb's parameters, taking effect at once (also before open()).
	MT32EMU_EXPORT void setDSeriesReverbSettings(const DSeriesReverbSettings &settings);
	// Sets new DAC input mode. See DACInputMode for details.
	MT32EMU_EXPORT void setDACInputMode(DACInputMode mode);
	// Returns current DAC input mode. See DACInputMode for details.
	MT32EMU_EXPORT DACInputMode getDACInputMode() const;
	// Sets new MIDI delay mode. See MIDIDelayMode for details.
	MT32EMU_EXPORT void setMIDIDelayMode(MIDIDelayMode mode);
	// Returns current MIDI delay mode. See MIDIDelayMode for details.
	MT32EMU_EXPORT MIDIDelayMode getMIDIDelayMode() const;

	// Sets output gain factor for synth output channels. Applied to all output samples and unrelated with the synth's Master volume,
	// it rather corresponds to the gain of the output analog circuitry of the hardware units. However, together with setReverbOutputGain()
	// it offers to the user a capability to control the gain of reverb and non-reverb output channels independently.
	MT32EMU_EXPORT void setOutputGain(float gain);
	// Returns current output gain factor for synth output channels.
	MT32EMU_EXPORT float getOutputGain() const;

	// Sets output gain factor for the reverb wet output channels. It rather corresponds to the gain of the output
	// analog circuitry of the hardware units. However, together with setOutputGain() it offers to the user a capability
	// to control the gain of reverb and non-reverb output channels independently.
	//
	// Note: We're currently emulate CM-32L/CM-64 reverb quite accurately and the reverb output level closely
	// corresponds to the level of digital capture. Although, according to the CM-64 PCB schematic,
	// there is a difference in the reverb analogue circuit, and the resulting output gain is 0.68
	// of that for LA32 analogue output. This factor is applied to the reverb output gain.
	MT32EMU_EXPORT void setReverbOutputGain(float gain);
	// Returns current output gain factor for reverb wet output channels.
	MT32EMU_EXPORT float getReverbOutputGain() const;

	// Swaps left and right output channels.
	MT32EMU_EXPORT void setReversedStereoEnabled(bool enabled);
	// Returns whether left and right output channels are swapped.
	MT32EMU_EXPORT bool isReversedStereoEnabled() const;

	// Allows to toggle the NiceAmpRamp mode.
	// In this mode, we want to ensure that amp ramp never jumps to the target
	// value and always gradually increases or decreases. It seems that real units
	// do not bother to always check if a newly started ramp leads to a jump.
	// We also prefer the quality improvement over the emulation accuracy,
	// so this mode is enabled by default.
	MT32EMU_EXPORT void setNiceAmpRampEnabled(bool enabled);
	// Returns whether NiceAmpRamp mode is enabled.
	MT32EMU_EXPORT bool isNiceAmpRampEnabled() const;

	// Allows to toggle the NicePanning mode.
	// Despite the Roland's manual specifies allowed panpot values in range 0-14,
	// the LA-32 only receives 3-bit pan setting in fact. In particular, this
	// makes it impossible to set the "middle" panning for a single partial.
	// In the NicePanning mode, we enlarge the pan setting accuracy to 4 bits
	// making it smoother thus sacrificing the emulation accuracy.
	// This mode is disabled by default.
	MT32EMU_EXPORT void setNicePanningEnabled(bool enabled);
	// Returns whether NicePanning mode is enabled.
	MT32EMU_EXPORT bool isNicePanningEnabled() const;

	// Mutes parts (bit n = part number n, 8 = rhythm): their partials go on playing, so that unmuting brings back what
	// sounds at that moment, but they are left out of the output. For solo and mute buttons.
	MT32EMU_EXPORT void setMutedParts(Bit32u partMask);
	// D-110 mode keeps a D-20's rhythm patterns and rhythm track (0A-0D xx xx). The track starts as the D-20's factory
	// one, which this restores: its 32 preset patterns P-11-P-48 in order, each twice (64 bars). Must be synchronised
	// with the rendering thread.
	MT32EMU_EXPORT void resetD20RhythmTrack();
	MT32EMU_EXPORT Bit32u getMutedParts() const;

	// ---- Part outputs (an extension, for plugin hosts with an output per part) ----
	// Takes effect on the next open(): every part then also has a stereo output of its own besides the mix (see render()
	// with part streams). A part in the output mask (bit n = part number n, 8 = rhythm) plays out of its own output
	// instead of the mix, with its pan and whatever its output assign (for the rhythm part, the keys'): the assign then
	// only says whether it also feeds the reverb (MIX + reverb), whose return stays in the mix. The D-110's MULTI 5/6
	// limit (lost while reverb is on) does not apply there. Everything else sounds as it does without part outputs.
	// Without `partStreams`, only the MULTI outputs' streams are built (a surround device has no use for the parts'),
	// and parts never play out of their own outputs.
	MT32EMU_EXPORT void setPartOutputsAvailable(bool available, bool partStreams = true);
	MT32EMU_EXPORT bool arePartOutputsAvailable() const;
	MT32EMU_EXPORT void setPartOutputMask(Bit32u partMask);
	MT32EMU_EXPORT Bit32u getPartOutputMask() const;
	// Whether the notes of this part play out of its own output.
	MT32EMU_EXPORT bool isPartOutputRouted(Bit32u partNumber) const;
	// With part outputs available: the D-110's MULTI outputs as outputs of their own (the plugins, a surround device).
	// Notes whose output assign is MULTI 1-6 (the rhythm part: per key) and whose part is not on its own output play out
	// of that MULTI output, mono at the level of the unit's jack (the centred partial's left and right) and dry, instead
	// of into the mix; MULTI 5 and 6 are not lost while reverb is on then.
	MT32EMU_EXPORT void setMultiOutputsEnabled(bool enabled);
	MT32EMU_EXPORT bool areMultiOutputsEnabled() const;
	// With the MULTI outputs as outputs of their own: MULTI 1+2, 3+4 and 5+6 as three stereo outputs instead of six mono
	// ones. Notes on either MULTI output of a pair play out of both its channels with their pan (as out of a part's own
	// output), dry. Notes started before a change keep the way they started.
	MT32EMU_EXPORT void setMultiPairsStereo(bool enabled);
	MT32EMU_EXPORT bool areMultiPairsStereo() const;

	// Allows to toggle the NicePartialMixing mode.
	// LA-32 is known to mix partials either in-phase (so that they are added)
	// or in counter-phase (so that they are subtracted instead).
	// In some cases, this quirk isn't highly desired because a pair of closely
	// sounding partials may occasionally cancel out.
	// In the NicePartialMixing mode, the mixing is always performed in-phase,
	// thus making the behaviour more predictable.
	// This mode is disabled by default.
	MT32EMU_EXPORT void setNicePartialMixingEnabled(bool enabled);
	// Returns whether NicePartialMixing mode is enabled.
	MT32EMU_EXPORT bool isNicePartialMixingEnabled() const;

	// Selects new type of the wave generator and renderer to be used during subsequent calls to open().
	// By default, RendererType_BIT16S is selected.
	// See RendererType for details.
	MT32EMU_EXPORT void selectRendererType(RendererType);
	// Returns previously selected type of the wave generator and renderer.
	// See RendererType for details.
	MT32EMU_EXPORT RendererType getSelectedRendererType() const;

	// Returns actual sample rate used in emulation of stereo analog circuitry of hardware units.
	// See comment for render() below.
	MT32EMU_EXPORT Bit32u getStereoOutputSampleRate() const;

	// Renders samples to the specified output stream as if they were sampled at the analog stereo output.
	// When AnalogOutputMode is set to ACCURATE (OVERSAMPLED), the output signal is upsampled to 48 (96) kHz in order
	// to retain emulation accuracy in whole audible frequency spectra. Otherwise, native digital signal sample rate is retained.
	// getStereoOutputSampleRate() can be used to query actual sample rate of the output signal.
	// The length is in frames, not bytes (in 16-bit stereo, one frame is 4 bytes). Uses NATIVE byte ordering.
	MT32EMU_EXPORT void render(Bit16s *stream, Bit32u len);
	// Same as above but outputs to a float stereo stream.
	MT32EMU_EXPORT void render(float *stream, Bit32u len);
	// The same mix, and with part outputs available, each part's own output into partStreams[part number]
	// (MAX_PART_COUNT entries, any of them NULL to leave that part out), interleaved stereo at the same rate.
	MT32EMU_EXPORT void render(float *stream, Bit32u len, float *const *partStreams);
	// As above, and the MULTI outputs into multiStreams (MULTI_OUTPUT_PAIRS entries, any NULL): MULTI 1 and 2, 3 and 4,
	// 5 and 6 as the left and right of interleaved pairs.
	MT32EMU_EXPORT void render(float *stream, Bit32u len, float *const *partStreams, float *const *multiStreams);

	// Renders samples to the specified output streams as if they appeared at the DAC entrance.
	// No further processing performed in analog circuitry emulation is applied to the signal.
	// NULL may be specified in place of any or all of the stream buffers to skip it.
	// The length is in samples, not bytes. Uses NATIVE byte ordering.
	MT32EMU_EXPORT void renderStreams(Bit16s *nonReverbLeft, Bit16s *nonReverbRight, Bit16s *reverbDryLeft, Bit16s *reverbDryRight, Bit16s *reverbWetLeft, Bit16s *reverbWetRight, Bit32u len);
	MT32EMU_EXPORT void renderStreams(const DACOutputStreams<Bit16s> &streams, Bit32u len);
	// Same as above but outputs to float streams.
	MT32EMU_EXPORT void renderStreams(float *nonReverbLeft, float *nonReverbRight, float *reverbDryLeft, float *reverbDryRight, float *reverbWetLeft, float *reverbWetRight, Bit32u len);
	MT32EMU_EXPORT void renderStreams(const DACOutputStreams<float> &streams, Bit32u len);

	// Returns true when there is at least one active partial, otherwise false.
	MT32EMU_EXPORT bool hasActivePartials() const;

	// Returns true if the synth is active and subsequent calls to render() may result in non-trivial output (i.e. silence).
	// The synth is considered active when either there are pending MIDI events in the queue, there is at least one active partial,
	// or the reverb is (somewhat unreliably) detected as being active.
	MT32EMU_EXPORT bool isActive();

	// Returns the maximum number of partials playing simultaneously.
	MT32EMU_EXPORT Bit32u getPartialCount() const;

	// Fills in current states of all the parts into the array provided. The array must have at least 9 entries to fit values for all the parts.
	// If the value returned for a part is true, there is at least one active non-releasing partial playing on this part.
	// This info is useful in emulating behaviour of LCD display of the hardware units.
	MT32EMU_EXPORT void getPartStates(bool *partStates) const;

	// Returns current states of all the parts as a bit set. The least significant bit corresponds to the state of part 1,
	// total of 9 bits hold the states of all the parts. If the returned bit for a part is set, there is at least one active
	// non-releasing partial playing on this part. This info is useful in emulating behaviour of LCD display of the hardware units.
	MT32EMU_EXPORT Bit32u getPartStates() const;

	// Fills in current states of all the partials into the array provided. The array must be large enough to accommodate states of all the partials.
	MT32EMU_EXPORT void getPartialStates(PartialState *partialStates) const;

	// Fills in current states of all the partials into the array provided. Each byte in the array holds states of 4 partials
	// starting from the least significant bits. The state of each partial is packed in a pair of bits.
	// The array must be large enough to accommodate states of all the partials (see getPartialCount()).
	MT32EMU_EXPORT void getPartialStates(Bit8u *partialStates) const;

	// Fills in information about currently playing notes on the specified part into the arrays provided. The arrays must be large enough
	// to accommodate data for all the playing notes. The maximum number of simultaneously playing notes cannot exceed the number of partials.
	// Argument partNumber should be 0..7 for Part 1..8, or 8 for Rhythm.
	// Returns the number of currently playing notes on the specified part.
	MT32EMU_EXPORT Bit32u getPlayingNotes(Bit8u partNumber, Bit8u *keys, Bit8u *velocities) const;

	// Returns name of the patch set on the specified part.
	// Argument partNumber should be 0..7 for Part 1..8, or 8 for Rhythm.
	MT32EMU_EXPORT const char *getPatchName(Bit8u partNumber) const;

	// Stores internal state of emulated synth into an array provided (as it would be acquired from hardware).
	MT32EMU_EXPORT void readMemory(Bit32u addr, Bit32u len, Bit8u *data);

	// ---- D-110 extensions (meaningful when a D-110 control ROM is loaded) ----

	// Returns true when the loaded control ROM uses the D-110 address map and behaviour.
	MT32EMU_EXPORT bool isD110Mode() const;
	// MIDI channel (0-15, 16 = off) on which program changes select patches instead of timbres.
	MT32EMU_EXPORT void setControlChannel(Bit8u channel);
	MT32EMU_EXPORT Bit8u getControlChannel() const;
	// SysEx device ID answered besides the MIDI channels 0x00-0x0F: unit number less one (0x10 = unit #17).
	MT32EMU_EXPORT void setDeviceID(Bit8u deviceID);
	MT32EMU_EXPORT Bit8u getDeviceID() const;
	// Copies patch memory I-11..I-88 (0-63), or card patches C-11..C-88 (64-127) when a card is inserted, into the
	// temporary areas, like a program change on the control channel. Must be synchronised with the rendering thread.
	MT32EMU_EXPORT void recallPatch(Bit8u patchNum);
	// Number of the patch last recalled or written: 0-63 internal, 64-127 card.
	MT32EMU_EXPORT Bit8u getCurrentPatch() const;
	// Sets the patch number shown as current without recalling it (e.g. after restoring saved memory).
	MT32EMU_EXPORT void setCurrentPatch(Bit8u patchNum);
	// Timbre number (0-127, A11-B88) last selected by program change on a part, or 0xFF if none since power-up
	// (see isPartProgramFromCard() and isPartProgramFromAlt() for the card's and the extra timbres).
	MT32EMU_EXPORT Bit8u getPartProgram(Bit8u partNumber) const;
	// MIDI volume and expression of a part, 0-100.
	MT32EMU_EXPORT Bit8u getPartMIDIVolume(Bit8u partNumber) const;
	MT32EMU_EXPORT Bit8u getPartExpression(Bit8u partNumber) const;
	// The range a part's pitch bend spans each way, in cents: its channel's (with the MIDI extensions) or its timbre's.
	MT32EMU_EXPORT Bit32u getPartPitchBendRangeCents(Bit8u partNumber) const;
	// Partials a part is playing right now, and the partials its reserve setting keeps for it
	// (the reserve counts out of 32 and scales with the partial count).
	MT32EMU_EXPORT Bit32u getPartActivePartialCount(Bit8u partNumber) const;
	MT32EMU_EXPORT Bit32u getPartReservedPartialCount(Bit8u partNumber) const;
	// Name of a tone (timbre) by absolute number: 0-63 group a, 64-127 group b, 128-191 memory (i), 192-255 rhythm (r);
	// D-110: 256-319 the card's (c), 320-447 the extra banks (d, e). Writes 10 characters and a terminating zero.
	MT32EMU_EXPORT void getToneName(Bit16u absToneNumber, char *name) const;
	// A tone's data (246 bytes, the timbre memory format) by absolute number, as getToneName() numbers them.
	MT32EMU_EXPORT bool readTone(Bit16u absToneNumber, Bit8u *data) const;

	// ---- 16-part mode (D-110 only) ----
	// Parts 9-15 besides parts 1-8 and rhythm: 15 melodic parts and rhythm, one for each MIDI channel. Takes effect on
	// the next open(). Parts 9-15 are numbered 9-15 (8 stays the rhythm part); their temporary areas are at 13 00 00
	// and 14 00 00, their partial reserves and MIDI channels at 11 00 00. Patches store parts 1-8 only.
	MT32EMU_EXPORT void setSixteenPartMode(bool enabled);
	// Number of parts including rhythm: 9, or 16 in 16-part mode.
	MT32EMU_EXPORT Bit32u getPartCount() const;
	// True while a part plays at least one non-releasing partial.
	MT32EMU_EXPORT bool isPartActive(Bit8u partNumber) const;

	// ---- D-110 memory card (M-256D) ----
	// With a card, tone group i/c reaches the card's tones in parts whose timbre came from the card, patches with
	// group c tones and program changes 64-127 on the control channel use the card, and write requests to the card
	// store there. The card's memory is at 18 00 00 (tones), 15 00 00 (timbres) and 16 00 00 (patches).
	MT32EMU_EXPORT void setCardInserted(bool inserted);
	MT32EMU_EXPORT bool isCardInserted() const;
	// Loads timbre 0-127 (A11-B88) of the internal memory, 128-255 (C-A11-C-B88) of the card, or 256-383 of the extra
	// banks (D11-E88, see setAltTimbre()) into a part (0-7, 9-15) like a program change. Must be synchronised with the
	// rendering thread.
	MT32EMU_EXPORT void selectPartTimbre(Bit8u partNumber, Bit16u timbreNumber);
	// True when the part's timbre (getPartProgram()) came from the card, or from the extra banks.
	MT32EMU_EXPORT bool isPartProgramFromCard(Bit8u partNumber) const;
	MT32EMU_EXPORT bool isPartProgramFromAlt(Bit8u partNumber) const;

	// ---- Extra tone banks d and e (D-110 only, an extension) ----
	// 128 more tones, d11-e88, which the application fills (e.g. with the MT-32's presets translated for the D-110).
	// A part plays them where its timbre temporary area selects tone group a or b with its dummy byte (07H) set to
	// PART_ALT_TONES; patches and timbre memory keep that flag. The extra timbres D11-E88 (selectPartTimbre() 256-383)
	// play them with a new timbre's settings. Until setAltTimbresEnabled(true), such parts play a and b instead.
	// Melodic parts keep the tone they have loaded until they select one again.
	MT32EMU_EXPORT bool setAltTimbre(Bit32u toneNumber, const Bit8u *data);  // 0-127 = d11-e88, 246 bytes
	MT32EMU_EXPORT void setAltTimbresEnabled(bool enabled);
	MT32EMU_EXPORT bool hasAltTimbres() const;

	// ---- D-20 performance mode (D-110 only) ----
	// Parts 1 (upper tone) and 2 (lower tone) play the current performance patch on one MIDI channel: the upper tone
	// alone (whole), both layered (dual) or divided at the split point (split), like a D-20 in performance mode. The
	// other melodic parts are switched off; the rhythm part stays. Program changes on the channel select performance
	// patches A11-B88 (patch memory 07 00 00); the current patch is the patch temporary area at 03 04 00, where
	// SysEx edits apply at once. Leaving the mode does not restore the parts. Must be synchronised with rendering.
	MT32EMU_EXPORT void setPerformanceMode(bool enabled, Bit8u channel);
	MT32EMU_EXPORT bool isPerformanceMode() const;
	MT32EMU_EXPORT Bit8u getPerformanceChannel() const;
	MT32EMU_EXPORT void recallPerformance(Bit8u patchNum);
	// Number (0-127, A11-B88) of the performance patch last recalled or written.
	MT32EMU_EXPORT Bit8u getCurrentPerformance() const;
	// Sets the performance patch number shown as current without recalling it (e.g. after restoring saved memory).
	MT32EMU_EXPORT void setCurrentPerformance(Bit8u patchNum);

	// ---- MIDI extensions beyond the original synths (off by default) ----
	// They make the synth behave more like a General MIDI or GS module. The pitch bend range belongs to the MIDI
	// channel: RPN 0 sets it (semitones up to 24, then CC 38 the cents), program changes and timbre edits leave it
	// alone, and it is 2 semitones at power-on and after a reset (without the extensions, parts bend by their timbre's
	// bender range, which RPN 0 changes until the next program change, as on the real units). GS/XG NRPNs 01H 20H/21H
	// and CC 74/71 offset each part's TVF cutoff and resonance; GS NRPNs 01H 63H/64H/66H and CC 73/75/72 its TVA and
	// TVF envelopes' attack, decay and release times, NRPNs 01H 08H/09H and CC 76/77 its pitch LFO's rate and depth
	// (all 40H = no change, kept by Reset All Controllers); CC 65, 5 and 84 give it portamento (on/off, time, the key
	// the next note glides from); RPN 1/2 set its fine tune and key shift; GM/GM2 System On, GS Reset and XG System On
	// reset the MIDI channels; GS MASTER TUNE (40 00 00) tunes the synth on top of its own master tune. Switching them
	// off clears what they set. Must be synchronised with the rendering thread once open.
	MT32EMU_EXPORT void setMIDIExtensionsEnabled(bool enabled);
	MT32EMU_EXPORT bool isMIDIExtensionsEnabled() const;
	// True for SysEx (F0 ... F7) that asks a GM-style module to reset its channels, which the MIDI extensions take:
	// GM/GM2 System On, GS Reset or the SC-88's mode set, XG System On.
	MT32EMU_EXPORT static bool isMIDIResetMessage(const Bit8u *sysex, Bit32u len);
	// Silences every part and resets what the MIDI channels carry (controllers, hold pedal, pitch bend, RPN/NRPN, what
	// the MIDI extensions set: the pitch bend range, the sound controllers' offsets, portamento, the GS master tune),
	// leaving the memory (timbres, levels, pans, channels) alone.
	// Must be synchronised with the rendering thread.
	MT32EMU_EXPORT void resetMIDIChannels();
	// D-110 mode: every part's level to 100 and pan to the centre (fine pans dropped), as GM and GS modules reset them;
	// the rhythm keys' own levels and pans stay. In D-20 performance mode the patch then sets parts 1 and 2 again. The
	// MIDI extensions' GM, GS and XG resets do this after resetMIDIChannels().
	// Must be synchronised with the rendering thread.
	MT32EMU_EXPORT void resetPartLevelsAndPans();
	// The GS master tune last received, in tenths of a cent (-1000..+1000; 0 without one).
	MT32EMU_EXPORT Bit32s getGSMasterTune() const;
	// D-110 mode keeps the master volume in the system area (10 00 16) at 100, as the unit has a volume knob instead.
	// Enabled (for MT-32 translation), SysEx sets it as on the MT-32; disabling it restores 100.
	// Must be synchronised with the rendering thread.
	MT32EMU_EXPORT void setMasterVolumeEnabled(bool enabled);
	MT32EMU_EXPORT bool isMasterVolumeEnabled() const;
	// D-110 extension for MT-32 translation with exact presets: replaces a preset tone (a11-b88 as 0-127, r01-r64 as
	// 192-255) with a tone in the timbre memory format (246 bytes), e.g. an MT-32 preset translated for the D-110.
	// Melodic parts keep the tone they have loaded until they select one again; rhythm keys reload it.
	// restorePresetTimbres() brings back the control ROM's presets.
	MT32EMU_EXPORT bool setPresetTimbre(Bit32u timbreNumber, const Bit8u *data);
	MT32EMU_EXPORT bool restorePresetTimbres();
}; // class Synth

} // namespace MT32Emu

#endif // #ifndef MT32EMU_SYNTH_H
