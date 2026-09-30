/* d110emu's D-series reverb model, part of its mt32emu and under the same licence.
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

#include <cmath>
#include <cstddef>
#include <cstring>

#include "internals.h"

#include "DSeriesReverb.h"
#include "Synth.h"

namespace MT32Emu {

namespace {

const float RATE = float(SAMPLE_RATE);
const float TWO_PI = 6.2831853f;
// Keeps the recursive filters out of denormals.
const float BIAS = 1e-20f;
// Output taps of each comb, as fractions of its length: 1 is the comb's output, less reads it earlier. The left and right
// taps differ, so the outputs are decorrelated as on the MT-32 family's chip.
const float TAP_L[DSeriesReverbType::COMB_COUNT] = {1.0f, 0.71f, 0.53f, 0.87f};
const float TAP_R[DSeriesReverbType::COMB_COUNT] = {0.61f, 1.0f, 0.83f, 0.47f};
// Below this, input and output count as silence (about -100 dB).
const float QUIET = 1e-5f;
// Where the reverbs' RT60 holds.
const float RT60_REFERENCE_HZ = 700.0f;

template <class T>
T clampValue(T value, T low, T high) {
	return value < low ? low : (value > high ? high : value);
}

Bit32u msToSamples(float ms) {
	const float samples = ms * RATE / 1000.0f;
	return samples < 1.0f ? 1 : Bit32u(samples + 0.5f);
}

// One-pole low-pass coefficient: y += coef * (x - y).
float lowPassCoef(float hz) {
	return 1.0f - std::exp(-TWO_PI * clampValue(hz, 20.0f, RATE * 0.49f) / RATE);
}

// A 2nd-order low-pass (bilinear, as in the RBJ cookbook) with Q 0.5: critically damped, two poles at the cutoff, the
// roll-off the D-20's reverbs show (-5 dB at 4 kHz, -12 dB at 8 kHz against 1 kHz), between a one-pole and a Butterworth.
struct LowPass2 {
	float b0, b1, b2, a1, a2;
	float x1, x2, y1, y2;

	LowPass2() : b0(1.0f), b1(0.0f), b2(0.0f), a1(0.0f), a2(0.0f), x1(0.0f), x2(0.0f), y1(0.0f), y2(0.0f) {}

	void setCutoff(float hz) {
		const float w = TWO_PI * clampValue(hz, 20.0f, RATE * 0.45f) / RATE;
		const float alpha = std::sin(w) / (2.0f * 0.5f);
		const float cosw = std::cos(w);
		const float a0 = 1.0f + alpha;
		b0 = (1.0f - cosw) * 0.5f / a0;
		b1 = (1.0f - cosw) / a0;
		b2 = b0;
		a1 = -2.0f * cosw / a0;
		a2 = (1.0f - alpha) / a0;
	}

	float process(float x) {
		const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		return y;
	}

	void reset() {
		x1 = x2 = y1 = y2 = 0.0f;
	}
};

float dbToGain(float db) {
	return db <= -60.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

// A delay line of a fixed capacity, used with a shorter length: next() returns what was stored `length` samples ago,
// then store() replaces it, as munt's RingBuffer.
struct Line {
	float *buffer;
	Bit32u capacity;
	Bit32u length;
	Bit32u index;

	Line() : buffer(NULL), capacity(0), length(1), index(0) {}

	float next() {
		if (++index >= length) index = 0;
		return buffer[index];
	}

	void store(float value) {
		buffer[index] = value;
	}

	// The value stored `delay` samples ago (1 to length - 1); 0 is the one just stored.
	float at(Bit32u delay) const {
		return buffer[(index + length - delay) % length];
	}

	void setLength(Bit32u newLength) {
		length = clampValue<Bit32u>(newLength, 1, capacity);
		if (index >= length) index = 0;
	}

	void clear() {
		if (buffer != NULL) std::memset(buffer, 0, capacity * sizeof(float));
		index = 0;
	}
};

} // namespace

namespace {

DSeriesReverbSettings makeDefaults() {
	DSeriesReverbSettings defaults;
	// Fitted to a recording of a real D-20 (reverb-kit/, tools/reverb_analysis.py): the reverb's level against the dry
	// sound, its decay per octave band, its spectrum and where it starts, for every type and Reverb Time at Reverb
	// Level 7, and Medium Hall at every Reverb Level. The level curve is the same for all types: level 0 is silent,
	// then -24.2, -17.0, -13.6, -9.1, -4.8 and -2.1 dB against level 7.
	static const float LEVEL_CURVE[8] = {-100.0f, -24.2f, -17.0f, -13.6f, -9.1f, -4.8f, -2.1f, 0.0f};
	struct Reverb {
		float level7Db, preDelayMs, bandwidthHz, dampingHz, diffusion;
		float allpassMs[3];
		float combMs[4];
		float rt60[8];
	};
	static const Reverb REVERBS[5] = {
		// Small Room: 0.21-0.69 s, starting about 30 ms after the sound
		{-6.2f, 3.5f, 4750.0f, 10000.0f, 0.5f, {23.1f, 16.9f, 3.1f}, {31.3f, 37.9f, 44.2f, 51.6f},
		 {0.212f, 0.235f, 0.236f, 0.259f, 0.308f, 0.371f, 0.480f, 0.691f}},
		// Medium Room: 0.24-0.97 s, from about 35 ms
		{-6.6f, 6.5f, 5050.0f, 16000.0f, 0.5f, {31.1f, 22.8f, 2.4f}, {50.3f, 61.1f, 71.7f, 83.9f},
		 {0.244f, 0.259f, 0.276f, 0.304f, 0.383f, 0.535f, 0.794f, 0.971f}},
		// Medium Hall: 0.29-1.35 s, from about 55 ms
		{-6.5f, 24.5f, 4950.0f, 8600.0f, 0.5f, {41.4f, 25.3f, 5.5f}, {73.4f, 88.7f, 101.3f, 113.5f},
		 {0.289f, 0.289f, 0.375f, 0.549f, 0.736f, 1.024f, 1.316f, 1.347f}},
		// Large Hall: 0.44-2.04 s (Reverb Time 7 and 8 alike), from about 75 ms
		{-7.5f, 46.0f, 5250.0f, 16000.0f, 0.5f, {41.4f, 25.3f, 5.5f}, {81.8f, 99.7f, 117.1f, 141.2f},
		 {0.436f, 0.491f, 0.584f, 0.774f, 1.051f, 1.423f, 2.038f, 1.967f}},
		// Plate: 0.23-2.23 s, brighter, building up from about 30 ms
		{-12.1f, 8.0f, 8950.0f, 16000.0f, 0.6f, {30.3f, 20.1f, 4.9f}, {70.6f, 79.3f, 88.7f, 110.6f},
		 {0.231f, 0.305f, 0.357f, 0.489f, 0.653f, 1.096f, 1.522f, 2.234f}},
	};
	// Delay 1-3: every Reverb Time is 1.3 times the one before. Delay 1 is one echo (left and right together) up to
	// 250 ms; Delay 2 repeats one of up to 400 ms, each repeat 7 dB down; Delay 3 is Delay 1's echo on the left and
	// twice it on the right, repeated from the right as the MT-32's tap delay does.
	struct Delay {
		float level7Db, longestLeftMs, rightRatio, feedback, bandwidthHz;
	};
	static const Delay DELAYS[3] = {
		{-9.6f, 250.0f, 1.0f, 0.0f, 9500.0f},
		{-10.3f, 400.0f, 1.0f, 0.61f, 10700.0f},
		{-10.4f, 250.0f, 2.0f, 0.61f, 10900.0f},
	};
	for (unsigned int t = 0; t < 8; t++) {
		DSeriesReverbType &type = defaults.types[t];
		std::memset(&type, 0, sizeof(type));
		type.delay = t >= 5;
		type.sendDb = 0.0f;
		type.width = 1.0f;
		type.size = 1.0f;
		type.diffusion = 0.5f;
		const float level7 = type.delay ? DELAYS[t - 5].level7Db : REVERBS[t].level7Db;
		for (unsigned int i = 0; i < 8; i++) type.wetDb[i] = i == 0 ? -60.0f : level7 + LEVEL_CURVE[i];
		if (!type.delay) {
			const Reverb &reverb = REVERBS[t];
			type.preDelayMs = reverb.preDelayMs;
			type.bandwidthHz = reverb.bandwidthHz;
			type.dampingHz = reverb.dampingHz;
			type.diffusion = reverb.diffusion;
			std::memcpy(type.allpassMs, reverb.allpassMs, sizeof(type.allpassMs));
			std::memcpy(type.combMs, reverb.combMs, sizeof(type.combMs));
			std::memcpy(type.rt60, reverb.rt60, sizeof(type.rt60));
			for (unsigned int i = 0; i < 8; i++) type.delayLMs[i] = type.delayRMs[i] = 100.0f;
			type.feedback = 0.3f;
		} else {
			const Delay &delay = DELAYS[t - 5];
			type.bandwidthHz = delay.bandwidthHz;
			type.dampingHz = 5000.0f;
			for (unsigned int i = 0; i < 8; i++) {
				type.delayLMs[i] = delay.longestLeftMs * std::pow(1.3f, float(i) - 7.0f);
				type.delayRMs[i] = type.delayLMs[i] * delay.rightRatio;
				type.rt60[i] = 1.0f;
			}
			type.feedback = delay.feedback;
			// Unused by delays, kept valid.
			type.preDelayMs = 0.0f;
			std::memcpy(type.allpassMs, REVERBS[1].allpassMs, sizeof(type.allpassMs));
			std::memcpy(type.combMs, REVERBS[1].combMs, sizeof(type.combMs));
		}
	}
	return defaults;
}

} // namespace

const DSeriesReverbSettings &DSeriesReverbSettings::getDefaults() {
	static const DSeriesReverbSettings defaults = makeDefaults();
	return defaults;
}

void DSeriesReverbSettings::clamp() {
	for (unsigned int t = 0; t < 8; t++) {
		DSeriesReverbType &type = types[t];
		type.sendDb = clampValue(type.sendDb, -40.0f, 12.0f);
		type.bandwidthHz = clampValue(type.bandwidthHz, 200.0f, 16000.0f);
		type.dampingHz = clampValue(type.dampingHz, 200.0f, 16000.0f);
		type.width = clampValue(type.width, 0.0f, 1.0f);
		for (unsigned int i = 0; i < 8; i++) {
			type.wetDb[i] = clampValue(type.wetDb[i], -60.0f, 12.0f);
			type.rt60[i] = clampValue(type.rt60[i], 0.05f, 30.0f);
			type.delayLMs[i] = clampValue(type.delayLMs[i], 1.0f, D_SERIES_REVERB_MAX_DELAY_MS);
			type.delayRMs[i] = clampValue(type.delayRMs[i], 1.0f, D_SERIES_REVERB_MAX_DELAY_MS);
		}
		type.preDelayMs = clampValue(type.preDelayMs, 0.0f, D_SERIES_REVERB_MAX_PRE_DELAY_MS);
		type.size = clampValue(type.size, 0.25f, 2.0f);
		for (unsigned int i = 0; i < DSeriesReverbType::ALLPASS_COUNT; i++) {
			type.allpassMs[i] = clampValue(type.allpassMs[i], 0.1f, D_SERIES_REVERB_MAX_ALLPASS_MS);
		}
		for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) {
			type.combMs[i] = clampValue(type.combMs[i], 1.0f, D_SERIES_REVERB_MAX_COMB_MS);
		}
		type.diffusion = clampValue(type.diffusion, 0.0f, 0.9f);
		type.feedback = clampValue(type.feedback, 0.0f, 0.95f);
	}
}

namespace {

// The model at the synth's sample rate, in floating point, whatever the renderer's sample type.
class DSeriesReverbCore {
public:
	DSeriesReverbCore() : memory(NULL), type(0), time(0), level(0), delayL(1), delayR(1), delayFeedback(0.0f), quietSamples(0), activeWindow(0) {
		settings = DSeriesReverbSettings::getDefaults();
		for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) {
			tapLeft[i] = tapRight[i] = 0;
			combFeedback[i] = 0.0f;
		}
		resetState();
		configure();
	}

	~DSeriesReverbCore() {
		close();
	}

	bool isOpen() const {
		return memory != NULL;
	}

	void open() {
		if (isOpen()) return;
		// Every line at its largest, so that no setting ever allocates (types change while rendering).
		const Bit32u preCapacity = msToSamples(D_SERIES_REVERB_MAX_PRE_DELAY_MS) + 1;
		const Bit32u allpassCapacity = msToSamples(D_SERIES_REVERB_MAX_ALLPASS_MS * 2.0f) + 1;
		const Bit32u combCapacity = msToSamples(D_SERIES_REVERB_MAX_COMB_MS * 2.0f) + 1;
		const Bit32u delayCapacity = msToSamples(D_SERIES_REVERB_MAX_DELAY_MS) + 2;
		const Bit32u total = preCapacity + DSeriesReverbType::ALLPASS_COUNT * allpassCapacity + DSeriesReverbType::COMB_COUNT * combCapacity + delayCapacity;
		memory = new float[total];
		float *next = memory;
		assign(preDelay, next, preCapacity);
		for (unsigned int i = 0; i < DSeriesReverbType::ALLPASS_COUNT; i++) assign(allpasses[i], next, allpassCapacity);
		for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) assign(combs[i], next, combCapacity);
		assign(delayLine, next, delayCapacity);
		delayLine.length = delayCapacity;
		configure();
		mute();
	}

	void close() {
		delete[] memory;
		memory = NULL;
	}

	void mute() {
		if (!isOpen()) return;
		preDelay.clear();
		for (unsigned int i = 0; i < DSeriesReverbType::ALLPASS_COUNT; i++) allpasses[i].clear();
		for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) combs[i].clear();
		delayLine.clear();
		resetState();
		quietSamples = activeWindow;
	}

	void setSettings(const DSeriesReverbSettings &newSettings) {
		const DSeriesReverbType &before = settings.types[type];
		settings = newSettings;
		settings.clamp();
		const DSeriesReverbType &after = settings.types[type];
		// A new structure garbles what the lines hold: start them afresh.
		bool structureChanged = before.delay != after.delay || before.size != after.size || before.preDelayMs != after.preDelayMs;
		for (unsigned int i = 0; i < DSeriesReverbType::ALLPASS_COUNT; i++) structureChanged |= before.allpassMs[i] != after.allpassMs[i];
		for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) structureChanged |= before.combMs[i] != after.combMs[i];
		configure();
		if (structureChanged) mute();
	}

	void setType(Bit8u newType) {
		newType &= 7;
		if (newType == type) return;
		type = newType;
		configure();
		mute();
	}

	void setParameters(Bit8u newTime, Bit8u newLevel) {
		time = newTime & 7;
		level = newLevel & 7;
		configure();
	}

	bool isActive() const {
		return isOpen() && quietSamples < activeWindow;
	}

	void process(float inL, float inR, float &outL, float &outR) {
		const float in = (inL + inR) * 0.5f * sendGain + BIAS;
		const float band = bandwidth.process(in);
		float left, right;
		if (settings.types[type].delay) {
			const Bit32u capacity = delayLine.capacity;
			const float tapL = delayLine.buffer[(delayWrite + capacity - delayL) % capacity];
			const float tapR = delayLine.buffer[(delayWrite + capacity - delayR) % capacity];
			feedbackState += dampingCoef * (tapR - feedbackState);
			delayLine.buffer[delayWrite] = band + delayFeedback * feedbackState;
			if (++delayWrite >= capacity) delayWrite = 0;
			left = tapL;
			right = tapR;
		} else {
			float x = preDelay.next();
			preDelay.store(band);
			for (unsigned int i = 0; i < DSeriesReverbType::ALLPASS_COUNT; i++) {
				const float delayed = allpasses[i].next();
				const float stored = x - diffusion * delayed;
				allpasses[i].store(stored);
				x = delayed + diffusion * stored;
			}
			left = right = 0.0f;
			for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) {
				Line &comb = combs[i];
				const float out = comb.next();
				combDamping[i] += dampingCoef * (out - combDamping[i]);
				comb.store(x + combFeedback[i] * combDamping[i]);
				left += tapLeft[i] == 0 ? out : comb.at(tapLeft[i]);
				right += tapRight[i] == 0 ? out : comb.at(tapRight[i]);
			}
			// Four combs of about equal energy: halve their sum.
			left *= 0.5f;
			right *= 0.5f;
		}
		const float mid = 0.5f * (left + right);
		const float side = 0.5f * (left - right) * width;
		outL = (mid + side) * wetGain;
		outR = (mid - side) * wetGain;
		const float loudest = std::fabs(in) > std::fabs(outL) ? (std::fabs(in) > std::fabs(outR) ? std::fabs(in) : std::fabs(outR))
		                                                       : (std::fabs(outL) > std::fabs(outR) ? std::fabs(outL) : std::fabs(outR));
		if (loudest > QUIET) {
			quietSamples = 0;
		} else if (quietSamples < activeWindow) {
			quietSamples++;
		}
	}

private:
	float *memory;
	DSeriesReverbSettings settings;
	Bit8u type, time, level;
	Line preDelay;
	Line allpasses[DSeriesReverbType::ALLPASS_COUNT];
	Line combs[DSeriesReverbType::COMB_COUNT];
	Line delayLine;
	Bit32u delayWrite;
	Bit32u delayL, delayR;
	Bit32u tapLeft[DSeriesReverbType::COMB_COUNT];   // 0 = the comb's output
	Bit32u tapRight[DSeriesReverbType::COMB_COUNT];
	float combFeedback[DSeriesReverbType::COMB_COUNT];
	float combDamping[DSeriesReverbType::COMB_COUNT];
	float sendGain, wetGain, width, diffusion;
	float dampingCoef, delayFeedback;
	LowPass2 bandwidth;
	float feedbackState;
	Bit32u quietSamples;
	Bit32u activeWindow;  // Silent for this long, the lines hold nothing audible

	static void assign(Line &line, float *&next, Bit32u capacity) {
		line.buffer = next;
		line.capacity = capacity;
		line.length = 1;
		line.index = 0;
		next += capacity;
	}

	void resetState() {
		delayWrite = 0;
		bandwidth.reset();
		feedbackState = 0.0f;
		for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) combDamping[i] = 0.0f;
	}

	// Derives lengths and coefficients from the settings, the type, the Reverb Time and Level.
	void configure() {
		const DSeriesReverbType &t = settings.types[type];
		sendGain = dbToGain(t.sendDb);
		wetGain = dbToGain(t.wetDb[level]);
		width = t.width;
		diffusion = t.diffusion;
		bandwidth.setCutoff(t.bandwidthHz);
		dampingCoef = lowPassCoef(t.dampingHz);
		if (!isOpen()) return;
		Bit32u window = 0;
		if (t.delay) {
			delayL = clampValue<Bit32u>(msToSamples(t.delayLMs[time]), 1, delayLine.capacity - 1);
			delayR = clampValue<Bit32u>(msToSamples(t.delayRMs[time]), 1, delayLine.capacity - 1);
			delayFeedback = t.feedback;
			// Echoes until the feedback has brought them 100 dB down.
			const Bit32u longest = delayL > delayR ? delayL : delayR;
			const float repeats = delayFeedback > 0.001f ? 5.0f / -std::log10(delayFeedback) : 1.0f;
			window = Bit32u(float(longest) * (repeats + 1.0f));
		} else {
			preDelay.setLength(msToSamples(t.preDelayMs) + 1);
			window = preDelay.length;
			Bit32u longestAllpass = 1;
			for (unsigned int i = 0; i < DSeriesReverbType::ALLPASS_COUNT; i++) {
				allpasses[i].setLength(msToSamples(t.allpassMs[i] * t.size));
				window += allpasses[i].length * 8;  // An allpass rings on for a few passes
				if (allpasses[i].length > longestAllpass) longestAllpass = allpasses[i].length;
			}
			const float rt60 = t.rt60[time];
			// The allpasses ring on by themselves (60 dB in about ten of their lengths at 0.5): a shorter decay than
			// that needs less diffusion, so that they die away within half of it.
			const float ringLimit = std::pow(10.0f, -3.0f * float(longestAllpass) / (0.5f * rt60 * RATE));
			if (diffusion > ringLimit) diffusion = ringLimit;
			// The loop low-pass takes a little off at the reference frequency: the feedback makes up for it there.
			const float w = TWO_PI * RT60_REFERENCE_HZ / RATE;
			const float pole = 1.0f - dampingCoef;
			const float dampingGain = dampingCoef / std::sqrt(1.0f - 2.0f * pole * std::cos(w) + pole * pole);
			Bit32u longestComb = 0;
			for (unsigned int i = 0; i < DSeriesReverbType::COMB_COUNT; i++) {
				Line &comb = combs[i];
				comb.setLength(msToSamples(t.combMs[i] * t.size));
				// Each pass through the comb loses 60 dB * length / RT60 at the reference frequency.
				const float gain = std::pow(10.0f, -3.0f * float(comb.length) / (rt60 * RATE)) / dampingGain;
				combFeedback[i] = gain > 0.9995f ? 0.9995f : gain;
				tapLeft[i] = TAP_L[i] >= 1.0f ? 0 : clampValue<Bit32u>(Bit32u(TAP_L[i] * float(comb.length)), 1, comb.length - 1);
				tapRight[i] = TAP_R[i] >= 1.0f ? 0 : clampValue<Bit32u>(Bit32u(TAP_R[i] * float(comb.length)), 1, comb.length - 1);
				if (comb.length > longestComb) longestComb = comb.length;
			}
			// Until the tail is 100 dB down.
			window += Bit32u(rt60 * RATE * 100.0f / 60.0f) + longestComb;
		}
		activeWindow = window;
	}
};

template <class Sample>
class DSeriesReverbModelImpl : public DSeriesReverbModel {
public:
	bool isOpen() const {
		return core.isOpen();
	}

	void open() {
		core.open();
	}

	void close() {
		core.close();
	}

	void mute() {
		core.mute();
	}

	void setParameters(Bit8u time, Bit8u level) {
		core.setParameters(time, level);
	}

	bool isActive() const {
		return core.isActive();
	}

	bool isMT32Compatible(const ReverbMode) const {
		return false;
	}

	void setSettings(const DSeriesReverbSettings &settings) {
		core.setSettings(settings);
	}

	void setType(Bit8u type) {
		core.setType(type);
	}

	bool process(const IntSample *inLeft, const IntSample *inRight, IntSample *outLeft, IntSample *outRight, Bit32u numSamples);
	bool process(const FloatSample *inLeft, const FloatSample *inRight, FloatSample *outLeft, FloatSample *outRight, Bit32u numSamples);

private:
	DSeriesReverbCore core;
};

template <>
bool DSeriesReverbModelImpl<IntSample>::process(const IntSample *inLeft, const IntSample *inRight, IntSample *outLeft, IntSample *outRight, Bit32u numSamples) {
	if (!core.isOpen()) {
		if (outLeft != NULL) Synth::muteSampleBuffer(outLeft, numSamples);
		if (outRight != NULL) Synth::muteSampleBuffer(outRight, numSamples);
		return true;
	}
	const float scale = 1.0f / 32768.0f;
	while ((numSamples--) > 0) {
		float left, right;
		core.process(float(*(inLeft++)) * scale, float(*(inRight++)) * scale, left, right);
		if (outLeft != NULL) *(outLeft++) = Synth::clipSampleEx(IntSampleEx(std::floor(left * 32768.0f + 0.5f)));
		if (outRight != NULL) *(outRight++) = Synth::clipSampleEx(IntSampleEx(std::floor(right * 32768.0f + 0.5f)));
	}
	return true;
}

template <>
bool DSeriesReverbModelImpl<IntSample>::process(const FloatSample *, const FloatSample *, FloatSample *, FloatSample *, Bit32u) {
	return false;
}

template <>
bool DSeriesReverbModelImpl<FloatSample>::process(const IntSample *, const IntSample *, IntSample *, IntSample *, Bit32u) {
	return false;
}

template <>
bool DSeriesReverbModelImpl<FloatSample>::process(const FloatSample *inLeft, const FloatSample *inRight, FloatSample *outLeft, FloatSample *outRight, Bit32u numSamples) {
	if (!core.isOpen()) {
		if (outLeft != NULL) Synth::muteSampleBuffer(outLeft, numSamples);
		if (outRight != NULL) Synth::muteSampleBuffer(outRight, numSamples);
		return true;
	}
	while ((numSamples--) > 0) {
		float left, right;
		core.process(*(inLeft++), *(inRight++), left, right);
		if (outLeft != NULL) *(outLeft++) = left;
		if (outRight != NULL) *(outRight++) = right;
	}
	return true;
}

} // namespace

DSeriesReverbModel *DSeriesReverbModel::createDSeriesReverbModel(const RendererType rendererType) {
	switch (rendererType) {
	case RendererType_BIT16S:
		return new DSeriesReverbModelImpl<IntSample>;
	case RendererType_FLOAT:
		return new DSeriesReverbModelImpl<FloatSample>;
	}
	return NULL;
}

} // namespace MT32Emu
