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

#ifndef MT32EMU_D_SERIES_REVERB_H
#define MT32EMU_D_SERIES_REVERB_H

#include "globals.h"
#include "internals.h"
#include "Enumerations.h"
#include "Types.h"
#include "BReverbModel.h"

namespace MT32Emu {

// One reverb type of the D-110, D-10 and D-20 (types 1-8: Small Room, Medium Room, Medium Hall, Large Hall, Plate,
// Delay 1-3). The program of the D-series' reverb chip is not known, so this is a model in physical units, to be tuned
// by ear or fitted to recordings of a real unit.
// - Types 1-5 (reverbs) follow the topology of the MT-32 family's reverb chip (see BReverbModel): a pre-delay behind an
//   input low-pass, allpass diffusers, then damped combs read at several taps for the left and right outputs. Their
//   decay is set per Reverb Time as RT60 at mid frequencies (700 Hz, between the 500 Hz and 1 kHz octaves, where
//   reverb times are measured); the loop low-pass shortens it for the highs.
// - Types 6-8 (delays) are a delay line with a left and a right tap per Reverb Time and damped feedback from the right.
struct DSeriesReverbType {
	static const unsigned int ALLPASS_COUNT = 3;
	static const unsigned int COMB_COUNT = 4;

	bool delay;           // Types 6-8: a delay instead of a reverb
	float sendDb;         // Input gain
	float bandwidthHz;    // Input low-pass (2nd order, critically damped): the D-20's reverbs roll off from about 4.5 kHz
	float dampingHz;      // Low-pass in the comb loops (reverbs) or the feedback (delays)
	float width;          // Stereo width: 0 mono, 1 as built
	float wetDb[8];       // Output gain per Reverb Level 0-7 (level 0 is silent on the units: -60 or below mutes)
	// Reverbs (types 1-5)
	float preDelayMs;
	float allpassMs[ALLPASS_COUNT];
	float diffusion;      // Allpass coefficient (0.5 on the MT-32 family's chip)
	float combMs[COMB_COUNT];
	float size;           // Scales the allpasses and combs
	float rt60[8];        // Decay to -60 dB at 700 Hz per Reverb Time 1-8, seconds
	// Delays (types 6-8)
	float delayLMs[8];    // Left tap per Reverb Time 1-8
	float delayRMs[8];    // Right tap
	float feedback;       // Taken from the right tap, 0-0.95
};

struct DSeriesReverbSettings {
	DSeriesReverbType types[8];

	// The starting point, before measurements of a real unit: shorter and drier than the MT-32 model.
	static const DSeriesReverbSettings &getDefaults();
	// Keeps every value in a range the model can process (delays within its buffers, gains finite).
	void clamp();
};

// Limits of the model's buffers.
static const float D_SERIES_REVERB_MAX_PRE_DELAY_MS = 250.0f;
static const float D_SERIES_REVERB_MAX_ALLPASS_MS = 100.0f;
static const float D_SERIES_REVERB_MAX_COMB_MS = 250.0f;
static const float D_SERIES_REVERB_MAX_DELAY_MS = 1500.0f;

class DSeriesReverbModel : public BReverbModel {
public:
	static DSeriesReverbModel *createDSeriesReverbModel(const RendererType rendererType);

	// Takes effect at once, for the current type, Reverb Time and Level.
	virtual void setSettings(const DSeriesReverbSettings &settings) = 0;
	// 0-7 (types 1-8). A change of type clears the buffers, as a new program would.
	virtual void setType(Bit8u type) = 0;
};

} // namespace MT32Emu

#endif // #ifndef MT32EMU_D_SERIES_REVERB_H
