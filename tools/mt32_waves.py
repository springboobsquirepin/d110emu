#!/usr/bin/env python3
"""Generates the MT-32 -> D-110 wave table (kWaves in src/Mt32Translator.cpp) from the D-110 ROMs.

Usage (from d110emu/):
  python3 tools/mt32_waves.py             prints the C++ table rows
  python3 tools/mt32_waves.py --measure   also measures the sounding pitch of each tuned D-110 wave (needs numpy)

The D-110 tunes each wave to a C (C2-C6) at key 60 and coarse pitch C4. NOTES below holds the measured notes: --measure
runs autocorrelation (YIN) and a harmonic product spectrum on the de-logged PCM ROM. Loops come out exactly on a C.
Where the two disagree by an octave (a weak fundamental, e.g. Drawbars), YIN's period and the D-110 presets, which
layer each wave in unison with synth partials, decide. d110tests confirms two of them by measuring the output. MT-32
melodic waves are taken as tuned to C4, because MT-32 presets play them in unison with synth partials at coarse C4
(as the D-110's own presets do with its waves), so a wave's correction is 60 minus its D-110 note. The bank-2
"Loop" waves repeat a bank-1 sample at the recorded rate (pitch 5000H), which the correction takes into account.
The MT-32 wave names follow common MT-32 editor lists; the D-110's are from its manual (PCM sound list).
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONTROL = os.path.join(HERE, '..', 'roms', 'CONTROLromcombo.bin')
PCM = os.path.join(HERE, '..', 'roms', 'PCMromcombo.bin')
PCM_TABLE = 0x8900  # 256 x {pos, len, pitch LSB, pitch MSB}

D110_BANK1 = (['Bass Drum-1', 'Bass Drum-2', 'Bass Drum-3', 'Snare Drum-1', 'Snare Drum-2', 'Snare Drum-3', 'Snare Drum-4',
               'Tom Tom-1', 'Tom Tom-2', 'High-Hat', 'High-Hat (Loop)', 'Crash Cymbal-1', 'Crash Cymbal-2 (Loop)',
               'Ride Cymbal-1', 'Ride Cymbal-2 (Loop)', 'Cup', 'China Cymbal-1', 'China Cymbal-2 (Loop)', 'Rim Shot',
               'Hand Clap', 'Mute High Conga', 'Conga', 'Bongo', 'Cowbell', 'Tambourine', 'Agogo', 'Claves',
               'Timbale High', 'Timbale Low', 'Cabasa', 'Timpani Attack', 'Timpani', 'Acoustic Piano High',
               'Acoustic Piano Low', 'Piano Forte Thump', 'Organ Percussion', 'Trumpet', 'Lips', 'Trombone', 'Clarinet',
               'Flute High', 'Flute Low', 'Steamer', 'Indian Flute', 'Breath', 'Vibraphone High', 'Vibraphone Low',
               'Marimba', 'Xylophone High', 'Xylophone Low', 'Kalimba', 'Wind Bell', 'Chime Bar', 'Hammer', 'Guiro',
               'Chink', 'Nails', 'Fretless Bass', 'Pull Bass', 'Slap Bass', 'Thump Bass', 'Acoustic Bass',
               'Electric Bass', 'Gut Guitar', 'Steel Guitar', 'Dirty Guitar', 'Pizzicato', 'Harp', 'Contrabass', 'Cello',
               'Violin-1', 'Violin-2', 'Koto', 'Drawbars (Loop)', 'High Organ (Loop)', 'Low Organ (Loop)',
               'Trumpet (Loop)', 'Trombone (Loop)', 'Sax-1 (Loop)', 'Sax-2 (Loop)', 'Reed (Loop)', 'Slap Bass (Loop)',
               'Acoustic Bass (Loop)', 'Electric Bass-1 (Loop)', 'Electric Bass-2 (Loop)', 'Gut Guitar (Loop)',
               'Steel Guitar (Loop)', 'Electric Guitar (Loop)', 'Clav (Loop)', 'Cello (Loop)', 'Violin (Loop)',
               'Electric Piano-1 (Loop)', 'Electric Piano-2 (Loop)', 'Harpsichord-1 (Loop)', 'Harpsichord-2 (Loop)',
               'Telephone Bell (Loop)', 'Female Voice-1 (Loop)', 'Female Voice-2 (Loop)', 'Male Voice-1 (Loop)',
               'Male Voice-2 (Loop)']
              + ['Spectrum-%d (Loop)' % i for i in range(1, 11)] + ['Noise (Loop)'] + ['Shot-%d' % i for i in range(1, 18)])
D110_BANK2 = ([n + '*' for n in D110_BANK1[:30]] + ['Loop-%d' % i for i in range(1, 65)]
              + ['Jam-%d (Loop)' % i for i in range(1, 35)])
D110 = D110_BANK1 + D110_BANK2

MT32_BANK1 = [
    'Ac. Bass Drum', 'Ac. Snare Drum', 'El. Snare Drum', 'Electric Tom', 'Closed Hihat', 'Open Hihat',
    'Crash Cymbal', 'Crash Cymbal (loop)', 'Ride cymbal', 'Rim Shot', 'Hand Clap', 'Muted Conga', 'Conga', 'Bongo',
    'Cowbell', 'Tambourine', 'Agogo Bell', 'Claves', 'Timbale', 'Cabasa', 'Keypress', 'Perc Organ', 'Trombone',
    'Trumpet', 'Breath Noise (loop)', 'Clarinet', 'Flute', 'Pan Pipes', 'Shakuhachi', 'Alto Sax', 'Baritone Sax',
    'Marimba', 'Vibraphone', 'Xylophone', 'Tubular Bells', 'Fingered Bass', 'Slap Bass', 'Picked Bass (loop)',
    'Acoustic Bass', 'Nylon Guitar', 'Steel Guitar', 'Pizzicato', 'Harp', 'Harpsichord (loop)', 'Bow string',
    'Violin', 'Timpani', 'Orchestra Hit', 'Flute 2', 'Organ (loop)', 'Bowed Glass (loop)', 'Telephone',
    'Bowed Glass', 'Spectrum 7 (loop)', 'Ac. Bass Drum #', 'Ac. Snare Drum #', 'El. Snare Drum #', 'Ac. Tom #',
    'Closed Hihat #', 'Open Hihat (loop) #', 'Crash Cymbal #', 'Crash Cymbal (loop) #', 'Ride cymbal #',
    'Rim shot #', 'Hand clap #', 'Mute Conga #', 'Conga #', 'Bongo #', 'Cowbell #', 'Tambourine #', 'Agogo #',
    'Claves #', 'Timbale #', 'Cabasa #', 'Bass Drum (loop)', 'Snare (loop)', 'El. Snare (loop)',
    'Electric Tom (loop)', 'Hihat (loop)', 'Crash Cymbal (loop)', 'Ride cymbal (loop)', 'Ride cymbal 2 (loop)',
    'Rim (loop)', 'Hand clap (loop)', 'Muted Conga (loop)', 'Conga (loop)', 'Bongo (loop)', 'Cowbell (loop)',
    'Tambourine (loop)', 'Agogo (loop)', 'Claves (loop)', 'Timbale (loop)', 'Cabasa (loop)', 'Keypress (loop)',
    'Perc Organ (loop)', 'Trombone (loop)', 'Trumpet (loop)', 'Clarinet (loop)', 'Flute (loop)',
    'Pan Pipes (loop)', 'Shakuhachi (loop)', 'Alto Sax (loop)', 'Baritone Sax (loop)', 'Marimba (loop)',
    'Vibraphone (loop)', 'Xylophone (loop)', 'Tubular Bells (loop)', 'Fingered Bass (loop)', 'Slap Bass (loop)',
    'Acoustic Bass (loop)', 'Nylon Guitar (loop)', 'Steel Guitar (loop)', 'Pizzicato (loop)', 'Harp (loop)',
    'Bow string (loop)', 'Violin (loop)', 'Timpani (loop)', 'Orchestra Hit (loop)', 'Flute 2 (loop)',
    'Perc. loop 1', 'Perc. loop 2', 'Orch&Perc loop', 'Wind&Perc loop', 'Guitar & Bass loop', 'Orchestra loop',
    'Perc. loop 3', 'Bass & Perc. loop', 'Bass & Snare loop',
]
MT32_BANK2 = [
    'Laugh #', 'Applause #', 'Windchime #', 'Crash #', 'Train #', 'Wind #', 'Bird #', 'Stream #', 'Door Creak #',
    'Scream #', 'Punch #', 'Footsteps #', 'Door Slam #', 'Car Start #', 'Aircraft #', 'Gun Shot #', 'Horse #',
    'Thunder #', 'Bubble #', 'Heartbeat #', 'Engine #', 'Tyre Screech #', 'Siren #', 'Helicopter #', 'Dog Bark #',
    'Car Pass #', 'Male Voice #', 'Machine Gun #', 'Starship #', 'Laugh (Loop) #', 'Applause (Loop) #',
    'Windchime (Loop) #', 'Crash (Loop) #', 'Train (Loop) #', 'Wind (Loop) #', 'Bird (Loop) #', 'Stream (Loop) #',
    'Door Creak (Loop) #', 'Scream (Loop) #', 'Punch (Loop) #', 'Footsteps (Loop) #', 'Door Slam (Loop) #',
    'Car Start (Loop) #', 'Aircraft (Loop) #', 'Gun Shot (Loop) #', 'Horse (Loop) #', 'Thunder (Loop) #',
    'Bubble (Loop) #', 'Heartbeat (Loop) #', 'Engine (Loop) #', 'Tyre Screech (Loop) #', 'Siren (Loop) #',
    'Helicopter (Loop) #', 'Dog Bark (Loop) #', 'Car Pass (Loop) #', 'Male Voice (Loop) #', 'Machine Gun (Loop) #',
    'Starship (Loop) #', 'Jam-1 (Loop)', 'Jam-2 (Loop)', 'Jam-3 (Loop)', 'Jam-4 (Loop)', 'Jam-5 (Loop)',
    'Jam-6 (Loop)', 'Jam-7 (Loop)', 'Jam-8 (Loop)', 'Jam-9 (Loop)', 'Jam-10 (Loop)', 'Jam-11 (Loop)',
    'Jam-12 (Loop)', 'Jam-13 (Loop)', 'Jam-14 (Loop)', 'Jam-15 (Loop)', 'Jam-16 (Loop)', 'Jam-17 (Loop)',
    'Jam-18 (Loop)', 'Jam-19 (Loop)', 'Jam-20 (Loop)', 'Jam-21 (Loop)', 'Jam-22 (Loop)', 'Jam-23 (Loop)',
    'Jam-24 (Loop)', 'Jam-25 (Loop)', 'Jam-26 (Loop)', 'Jam-27 (Loop)', 'Jam-28 (Loop)', 'Jam-29 (Loop)',
    'Jam-30 (Loop)', 'Jam-31 (Loop)', 'Jam-32 (Loop)', 'Jam-33 (Loop)', 'Jam-34 (Loop)', 'Jam-35 (Loop)',
    'Jam-36 (Loop)', 'Jam-37 (Loop)', 'Jam-38 (Loop)', 'Jam-39 (Loop)', 'Jam-40 (Loop)', 'Shot-1', 'Shot-2',
    'Shot-3', 'Shot-4', 'Shot-5', 'Shot-6', 'Shot-7', 'Shot-8', 'Shot-9', 'Shot-10', 'Shot-11', 'Shot-12',
    'Shot-13', 'Shot-14', 'Shot-15', 'Shot-16', 'Shot-17', 'Shot-18', 'Shot-19', 'Shot-20', 'Shot-21', 'Shot-22',
    'Shot-23', 'Shot-24', 'Shot-25', 'Shot-26', 'Alto Sax', 'Shakuhachi', 'Marimba', 'Dog Bark',
]

# Sounding note of the tuned D-110 waves at key 60, coarse pitch 36 (from --measure).
NOTES = {
    'Acoustic Piano High': 72, 'Acoustic Piano Low': 48, 'Organ Percussion': 48, 'Trumpet': 72, 'Trombone': 72,
    'Clarinet': 72, 'Flute High': 84, 'Flute Low': 72, 'Steamer': 72, 'Indian Flute': 72, 'Breath': 72,
    'Vibraphone High': 72, 'Vibraphone Low': 60, 'Marimba': 48, 'Xylophone High': 84, 'Xylophone Low': 72,
    'Wind Bell': 84, 'Fretless Bass': 48, 'Pull Bass': 48, 'Slap Bass': 48, 'Thump Bass': 48, 'Acoustic Bass': 48,
    'Electric Bass': 48, 'Gut Guitar': 60, 'Steel Guitar': 60, 'Pizzicato': 72, 'Harp': 84, 'Contrabass': 48,
    'Cello': 48, 'Violin-1': 72, 'Violin-2': 72, 'Koto': 60, 'Drawbars (Loop)': 48, 'High Organ (Loop)': 72,
    'Low Organ (Loop)': 60, 'Trumpet (Loop)': 60, 'Trombone (Loop)': 48, 'Sax-1 (Loop)': 48, 'Sax-2 (Loop)': 60,
    'Reed (Loop)': 48, 'Slap Bass (Loop)': 48, 'Acoustic Bass (Loop)': 48, 'Electric Bass-1 (Loop)': 48,
    'Electric Bass-2 (Loop)': 48, 'Gut Guitar (Loop)': 60, 'Steel Guitar (Loop)': 60, 'Electric Guitar (Loop)': 60,
    'Clav (Loop)': 48, 'Cello (Loop)': 60, 'Violin (Loop)': 72, 'Electric Piano-1 (Loop)': 60,
    'Electric Piano-2 (Loop)': 36, 'Harpsichord-1 (Loop)': 72, 'Harpsichord-2 (Loop)': 48,
    'Telephone Bell (Loop)': 72, 'Female Voice-1 (Loop)': 60, 'Female Voice-2 (Loop)': 60,
    'Male Voice-1 (Loop)': 60, 'Male Voice-2 (Loop)': 60,
}

# MT-32 wave -> (D-110 wave, correction, bank-1 stand-in for tones translated byte by byte[, fallback]).
# correction: None = none (drums and effects play at the recorded rate on both units); T = the MT-32 wave is tuned to
# C4, so 60 minus the D-110 wave's note; a number = semitones. The numbers with two decimals are exact: --identical
# finds these MT-32 samples bit for bit (up to level) in the D-110's PCM ROM, and the correction is the difference of
# the two control ROMs' pitch words. The others were matched by spectrum (see the notes).
# fallback: (D-110 wave, correction) for a partial whose corrected coarse pitch would leave 0-96, e.g. the loop of
# the MT-32's Tubular Bells, which plays its sample 57.5 semitones below the D-110's Loop-43.
# An MT-32 loop repeats its whole sample, like the D-110's Loop-n. Where the samples differ, Loop-n is used only when
# it repeats about as slowly as the MT-32's loop (a few Hz: a steady sound), otherwise a sustain loop.
T = 'tuned'
MAPPING = [
 # 0-19: drums, at the recorded rate
 ('Bass Drum-1', None, None), ('Snare Drum-1', None, None),
 ('Snare Drum-1', 4.0, None),               # 2 El. Snare (Elec Perc 1/2): Snare Drum-1 4 semitones up, closer than Snare Drum-2
                                            # (the user's choice; the drum kit's El. Snare # stays, see 56)
 ('Tom Tom-2', None, None),
 ('High-Hat', None, None), ('High-Hat (Loop)', None, None), ('Crash Cymbal-1', None, None), ('Crash Cymbal-2 (Loop)', None, None),
 ('Ride Cymbal-1', None, None), ('Rim Shot', None, None), ('Hand Clap', None, None), ('Mute High Conga', None, None),
 ('Conga', None, None), ('Bongo', None, None), ('Cowbell', None, None), ('Tambourine', None, None),
 ('Agogo', None, None), ('Claves', None, None), ('Timbale High', None, None), ('Cabasa', None, None),
 # 20-53: melodic waves and effects
 ('Piano Forte Thump', -12.00, None),       # 20 Keypress: identical
 ('Organ Percussion', 11.84, None),         # 21 identical
 ('Trombone', T, None), ('Trumpet', T, None),
 ('Noise (Loop)', None, None),              # 24 bright noise on both at about the recorded rate (half-octave bands within 3 dB from 700 Hz up)
 ('Clarinet', T, None), ('Flute Low', T, None),
 ('Steamer', -12.08, None),                 # 27 Pan Pipes: identical
 ('Breath', 0.43, None),                    # 28 Shakuhachi: identical
 ('Sax-2 (Loop)', T, None), ('Sax-1 (Loop)', T, None), ('Marimba', T, None),
 ('Vibraphone Low', T, None),               # 32 the MT-32's one vibraphone for the D-series' High and Low: Low is closer
 ('Xylophone Low', T, None),
 ('Wind Bell', -36.34, None),               # 34 Tubular Bells: identical
 ('Electric Bass', T, None),
 ('Thump Bass', T, None),                   # 36 Slap Bass: Thump Bass is much closer (the user's choice)
 ('Electric Piano-2 (Loop)', T, None),      # 37 Picked Bass loop: less bright and closer than the matching Electric Bass-1
                                            # (Loop) (the user's choice); sounding C2, so +24, an octave above that one's +12
 ('Acoustic Bass', 11.98, None),            # 38 identical
 ('Gut Guitar', T, None),
 ('Acoustic Piano Low', T, None),           # 40 Steel Gt: Acoustic Piano Low is closer (the user's choice); sounding C3, so +12
 ('Pizzicato', T, None), ('Harp', T, None),
 ('Harpsichord-2 (Loop)', 12.08, None),     # 43 identical
 ('Contrabass', T, None),
 ('Violin-1', -12.93, None),                # 45 identical
 ('Timpani Attack', 12.0, None),            # 46 the MT-32's one timpani merges attack and body; Timpani Attack keeps the percussive
                                            # hit (the user's choice). C4, against the D-110's untuned timpani (spectrum)
 ('Violin-2', None, None),                  # 47 Orchestra Hit: no D-series sample; Violin-2's attack stands in
 ('Indian Flute', -12.12, None),            # 48 identical
 ('Drawbars (Loop)', 0.12, None),           # 49 Organ: identical
 ('Spectrum-2 (Loop)', -21.86, None),       # 50 Bowed Glass loop: identical
 ('Telephone Bell (Loop)', -14.30, None),   # 51 identical
 ('Spectrum-6 (Loop)', -27.03, None),       # 52 Bowed Glass: identical
 ('Spectrum-7 (Loop)', -18.0, None),        # 53 Spectrum-7's waveform, stored 2.5% faster, not a cymbal (the user heard it). The
                                            # pitch words give -18.18 and the stretch +0.43, but the partial keeps the MT-32's fine
                                            # tune: a fractional correction makes the loop repeat at another rate (the user's choice)
 # 54-73: fixed-pitch drums
 ('Bass Drum-1*', None, 'Bass Drum-1'), ('Snare Drum-1*', None, 'Snare Drum-1'),
 ('Snare Drum-1*', 4.0, 'Snare Drum-1'),   # 56 El. Snare #, the same sample as 2: in tones (VOICE_MT.DAT's E-TOM) as 2, Snare
                                            # Drum-1 4 up (the user's choice); the MT-32's rhythm timbres (R6 Elec SD) keep Snare
                                            # Drum-2* (kDrumKitWaves in Mt32Translator.cpp)
 ('Tom Tom-2*', None, 'Tom Tom-2'),  # 57 Ac. Tom: the same sample as Electric Tom, the D-110's Tom Tom-2 (identical)
 ('High-Hat*', None, 'High-Hat'), ('High-Hat (Loop)*', None, 'High-Hat (Loop)'), ('Crash Cymbal-1*', None, 'Crash Cymbal-1'), ('Crash Cymbal-2 (Loop)*', None, 'Crash Cymbal-2 (Loop)'),
 ('Ride Cymbal-1*', None, 'Ride Cymbal-1'), ('Rim Shot*', None, 'Rim Shot'), ('Hand Clap*', None, 'Hand Clap'), ('Mute High Conga*', None, 'Mute High Conga'),
 ('Conga*', None, 'Conga'), ('Bongo*', None, 'Bongo'), ('Cowbell*', None, 'Cowbell'), ('Tambourine*', None, 'Tambourine'),
 ('Agogo*', None, 'Agogo'), ('Claves*', None, 'Claves'), ('Timbale High*', None, 'Timbale High'), ('Cabasa*', None, 'Cabasa'),
 # 74-93: drum loops: the D-110's Loop-n of the same drum
 ('Loop-1', None, 'Bass Drum-1'), ('Loop-3', None, 'Snare Drum-1'),
 ('Loop-3', 4.0, 'Snare Drum-1'),           # 76 El. Snare loop: as 2
 ('Loop-6', None, 'Tom Tom-2'),
 ('Loop-7', None, 'High-Hat (Loop)'), ('Loop-8', None, 'Crash Cymbal-2 (Loop)'), ('Loop-9', None, 'Ride Cymbal-2 (Loop)'), ('Ride Cymbal-2 (Loop)', None, None),
 ('Loop-13', None, 'Rim Shot'), ('Loop-14', None, 'Hand Clap'), ('Loop-16', None, 'Mute High Conga'), ('Loop-17', None, 'Conga'),
 ('Loop-18', None, 'Bongo'), ('Loop-19', None, 'Cowbell'), ('Loop-20', None, 'Tambourine'), ('Loop-22', None, 'Agogo'),
 ('Loop-23', None, 'Claves'), ('Loop-24', None, 'Timbale High'), ('Loop-26', None, 'Cabasa'),
 ('Loop-29', -24.00, 'Piano Forte Thump', ('Piano Forte Thump', -12.00)),  # 93 Keypress loop: identical
 # 94-118: loops of the melodic samples, which loop the whole sample like the D-110's Loop-n
 ('Loop-30', 0.81, 'Organ Percussion'),                           # 94 identical
 ('Loop-33', T, 'Trombone', ('Trombone (Loop)', T)),
 ('Loop-31', T, 'Trumpet', ('Trumpet (Loop)', T)),
 ('Loop-36', T, 'Clarinet', ('Reed (Loop)', T)),
 ('Loop-37', T, 'Flute High', ('Flute Low', T)),
 ('Loop-38', -11.49, 'Steamer'),                                  # 99 identical
 ('Loop-35', -5.00, 'Breath'),                                    # 100 identical
 ('Sax-2 (Loop)', T, None), ('Sax-1 (Loop)', T, None),
 ('Loop-41', T, 'Marimba'),
 ('Loop-40', T, 'Vibraphone Low'),
 ('Loop-42', T, 'Xylophone High', ('Xylophone Low', T)),
 ('Loop-43', -57.53, 'Wind Bell', ('Wind Bell', -36.34)),         # 106 identical
 # The basses' Loop-n would have to run so fast that the loop itself buzzes (50 Hz), where the MT-32's repeats
 # below 12 Hz: their sustain loops sound closer.
 ('Electric Bass-1 (Loop)', T, None), ('Slap Bass (Loop)', T, None),
 ('Loop-52', 10.82, 'Acoustic Bass'),                             # 109 identical
 ('Loop-56', T, 'Gut Guitar'), ('Loop-57', T, 'Steel Guitar'), ('Loop-59', T, 'Pizzicato', ('Pizzicato', T)),
 ('Loop-60', T, 'Harp', ('Harp', T)), ('Loop-61', T, 'Contrabass'),
 ('Loop-63', -12.56, 'Violin-1'),                                 # 115 identical
 ('Loop-28', 12.0, 'Timpani Attack'),
 ('Violin (Loop)', None, None),                                   # 117 Orchestra Hit loop, like 47
 ('Loop-34', -23.41, 'Indian Flute', ('Indian Flute', -12.12)),   # 118 identical
 # 119-127: loops of combined sounds
 ('Jam-8 (Loop)', None, 'Noise (Loop)'), ('Jam-1 (Loop)', None, 'Noise (Loop)'), ('Jam-6 (Loop)', None, 'Noise (Loop)'), ('Jam-2 (Loop)', None, 'Noise (Loop)'),
 ('Jam-18 (Loop)', None, 'Noise (Loop)'), ('Jam-13 (Loop)', None, 'Noise (Loop)'), ('Jam-7 (Loop)', None, 'Noise (Loop)'), ('Jam-3 (Loop)', None, 'Noise (Loop)'),
 ('Jam-21 (Loop)', None, 'Noise (Loop)'),
]
assert len(MAPPING) == 128, len(MAPPING)

# CM-32L bank 2: 29 sound effects, the same looped, Jam-1..40, Shot-1..26, then four instruments.
SFX = {'Windchime #': 'Wind Bell', 'Crash #': 'Crash Cymbal-1', 'Punch #': 'Bass Drum-1', 'Door Slam #': 'Snare Drum-1',
       'Gun Shot #': 'Snare Drum-3', 'Heartbeat #': 'Bass Drum-2', 'Footsteps #': 'Tom Tom-1', 'Male Voice #': 'Male Voice-1 (Loop)'}
SFX_LOOPS = {'Windchime (Loop) #': 'Loop-43', 'Crash (Loop) #': 'Crash Cymbal-2 (Loop)', 'Heartbeat (Loop) #': 'Loop-2',
             'Footsteps (Loop) #': 'Loop-6', 'Machine Gun (Loop) #': 'Loop-3', 'Male Voice (Loop) #': 'Male Voice-1 (Loop)',
             'Punch (Loop) #': 'Loop-1', 'Gun Shot (Loop) #': 'Loop-5'}


def bank2_mapping():
    result = []
    for k, name in enumerate(MT32_BANK2):
        if k < 29:
            result.append((SFX.get(name, 'Noise (Loop)'), None, None))
        elif k < 58:
            target = SFX_LOOPS.get(name, 'Noise (Loop)')
            result.append((target, None, 'Noise (Loop)' if target.startswith('Loop') else None))
        elif k < 98:
            result.append(('Jam-%d (Loop)' % ((k - 58) % 34 + 1), None, 'Noise (Loop)'))
        elif k < 124:
            result.append(('Shot-%d' % ((k - 98) % 17 + 1), None, None))
        else:
            # Taken as the bank-1 samples of the same names (no CM-32L PCM ROM was at hand to check).
            result.append({124: ('Sax-2 (Loop)', T, None), 125: ('Breath', 0.43, None), 126: ('Marimba', T, None),
                           127: ('Noise (Loop)', None, None)}[k])
    return result


def wave_entries(rom):
    entries = []
    for i in range(256):
        pos, length, lsb, msb = rom[PCM_TABLE + i * 4:PCM_TABLE + i * 4 + 4]
        entries.append({'addr': pos * 0x800, 'len': 0x800 << ((length & 0x70) >> 4), 'loop': bool(length & 0x80),
                        'pitch': (msb << 8) | lsb, 'pos': pos})
    return entries


def correction(wave, entries):
    """Semitones for the D-110 wave to sound C4 at key 60 like a tuned MT-32 wave."""
    name = D110[wave]
    if name in NOTES:
        return 60 - NOTES[name]
    if wave >= 158 and wave < 222:  # Loop-n: a bank-1 sample repeated at the recorded rate
        for j in range(128):
            if entries[j]['pos'] == entries[wave]['pos'] and not entries[j]['loop'] and D110[j] in NOTES:
                return 60 - NOTES[D110[j]] - 12.0 * (20480 - entries[j]['pitch']) / 4096.0
    return 0.0


def measure(rom, entries):
    import numpy as np
    linear = decode_pcm(PCM)
    def yin(x, loop):
        # First dip of the cumulative mean normalised difference: the true period even when the fundamental is weak.
        if loop:
            x = np.concatenate([x, x, x])
        max_lag = min(1280, len(x) // 2 - 1)
        window = len(x) - max_lag
        d = np.array([np.sum((x[:window] - x[lag:lag + window]) ** 2) for lag in range(1, max_lag + 1)])
        cmnd = d * np.arange(1, max_lag + 1) / np.maximum(np.cumsum(d), 1e-12)
        for lag in range(12, max_lag - 1):
            if cmnd[lag - 1] < 0.15 and cmnd[lag - 1] <= cmnd[lag]:
                return 32000.0 / lag, cmnd[lag - 1]
        lag = 12 + int(np.argmin(cmnd[11:]))
        return 32000.0 / lag, cmnd[lag - 1]

    def hps(x):
        n = 1 << 16
        spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
        freqs = np.fft.rfftfreq(n, 1 / 32000.0)
        product = spec.copy()
        for h in (2, 3, 4):
            product[:len(spec[::h])] *= spec[::h]
        lo, hi = np.searchsorted(freqs, 20), np.searchsorted(freqs, 2000)
        return freqs[lo + np.argmax(product[lo:hi])]

    note = lambda f, e: 69 + 12 * math.log2(f * 2 ** ((e['pitch'] - 20480) / 4096.0) / 440.0)
    print('Sounding note at key 60, coarse 36: autocorrelation (YIN, with its aperiodicity) and harmonic product spectrum')
    for i in range(111):
        e = entries[i]
        x = linear[e['addr']:e['addr'] + e['len']]
        if not e['loop']:
            x = x[e['len'] // 3:]
        x = x[:8192] - np.mean(x[:8192])
        f_yin, aperiodicity = yin(x, e['loop'])
        f_hps = hps(x)
        used = ' NOTES %d' % NOTES[D110[i]] if D110[i] in NOTES else ''
        print('%3d %-26s %s pitch %5d  YIN %6.2f (%.2f)  HPS %6.2f%s' % (
            i + 1, D110[i], 'L' if e['loop'] else ' ', e['pitch'], note(f_yin, e), aperiodicity, note(f_hps, e), used))


def offset_of(target, spec, entries):
    wave = D110.index(target)
    if spec is None:
        return wave, 0.0
    return wave, correction(wave, entries) if spec == T else float(spec)


def split(offset):
    coarse = int(math.floor(offset + 0.5))
    return coarse, int(round((offset - coarse) * 100))


def main():
    rom = open(CONTROL, 'rb').read()
    entries = wave_entries(rom)
    if '--measure' in sys.argv:
        measure(rom, entries)
        print()
    if '--identical' in sys.argv:
        identical(entries, sys.argv[sys.argv.index('--identical') + 1])
        print()
    fallbacks = []
    table = [(1, MAPPING, MT32_BANK1), (2, bank2_mapping(), MT32_BANK2)]
    print('const Mt32Translator::WaveMapping kWaves[256] = {')
    for bank, mapping, names in table:
        for k, row in enumerate(mapping):
            target, spec, stand_in = row[:3]
            wave, offset = offset_of(target, spec, entries)
            coarse, fine = split(offset)
            bank1 = D110.index(stand_in) if stand_in else wave
            assert bank1 < 128, (k, target)
            mt32 = k + (128 if bank == 2 else 0)
            print('    {%3d, %3d, %3d, %3d},  // %3d %-22s -> %s%s' % (
                wave, coarse, fine, bank1, mt32, names[k][:22],
                'b%d-%03d %s' % (wave // 128 + 1, wave % 128 + 1, D110[wave]),
                (' (bank 1: %s)' % D110[bank1]) if bank1 != wave else ''))
            if len(row) > 3:
                fallbacks.append((mt32, names[k]) + offset_of(row[3][0], row[3][1], entries))
    print('};')
    print()
    print('const Mt32Translator::WaveFallback kWaveFallbacks[] = {')
    for mt32, name, wave, offset in fallbacks:
        coarse, fine = split(offset)
        print('    {%3d, %3d, %3d, %3d},  // %-22s -> %s' % (mt32, wave, coarse, fine, name[:22],
              'b%d-%03d %s' % (wave // 128 + 1, wave % 128 + 1, D110[wave])))
    print('};')


def identical(entries, mt32_dir):
    """Lists MT-32 samples that the D-110's PCM ROM holds too (same data up to level) and their exact correction."""
    import numpy as np
    from collections import defaultdict
    mt32_rom = open(os.path.join(mt32_dir, 'MT32_CONTROL.ROM'), 'rb').read()
    mt32_table = 0x8100 if len(mt32_rom) == 131072 else 0x3000  # v2.04 / CM-32L, or 1.0x
    mt32_entries = []
    for i in range(128):
        pos, length, lsb, msb = mt32_rom[mt32_table + i * 4:mt32_table + i * 4 + 4]
        mt32_entries.append({'addr': pos * 0x800, 'len': 0x800 << ((length & 0x70) >> 4), 'pitch': (msb << 8) | lsb})
    def features(path):
        # Sign and quantised log-magnitude steps, which a change of level leaves alone.
        linear = decode_pcm(path)
        log = np.log2(np.abs(linear) + 1e-3)
        return np.round(np.diff(log) * 16).astype(np.int32) * 2 + (linear[1:] < 0)
    mt32, d110 = features(os.path.join(mt32_dir, 'MT32_PCM.ROM')), features(PCM)
    width = 24
    index = defaultdict(list)
    windows = np.lib.stride_tricks.sliding_window_view(d110, width)
    for position in range(len(windows)):
        index[windows[position].tobytes()].append(position)
    print('MT-32 samples found in the D-110 PCM ROM, and the correction that plays them alike:')
    for i, e in enumerate(mt32_entries):
        votes = defaultdict(int)
        for offset in range(32, min(e['len'], 4000) - width, 97):
            for position in index.get(mt32[e['addr'] + offset:e['addr'] + offset + width].tobytes(), []):
                votes[position - offset] += 1
        if not votes:
            continue
        start, count = max(votes.items(), key=lambda v: v[1])
        if count < 3:
            continue
        waves = [j for j in range(128) if entries[j]['addr'] <= start + 64 < entries[j]['addr'] + entries[j]['len']]
        if not waves:
            continue
        j = min(waves, key=lambda j: entries[j]['len'])
        print('  %3d %-22s = %3d %-24s %+7.2f' % (i, MT32_BANK1[i], j, D110[j], 12.0 * (e['pitch'] - entries[j]['pitch']) / 4096))


def decode_pcm(path):
    import numpy as np
    raw = np.frombuffer(open(path, 'rb').read(), dtype=np.uint8).reshape(-1, 2)
    s, c = raw[:, 0].astype(np.int32), raw[:, 1].astype(np.int32)
    order = [0, 9, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 8]  # Address/data scrambling, as mt32emu
    word = np.zeros(len(s), dtype=np.int32)
    for u in range(15):
        o = order[u]
        bit = (s >> (7 - o)) & 1 if o < 8 else (c >> (7 - (o - 8))) & 1
        word |= bit << (15 - u)
    word = word.astype(np.uint16).astype(np.int16).astype(np.int32)
    log = np.minimum((32787 - (word & 32767)) * 2, 65535)  # LA32 log samples
    return np.power(2.0, 13.0 - log / 4096.0) * np.where(word < 0, -1.0, 1.0)


if __name__ == '__main__':
    main()
