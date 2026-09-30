Reverb recording kit for d110emu
================================

These MIDI files play test sounds through every reverb setting of a real D-110 or D-10/D-20. A recording of them
lets d110emu measure the unit's reverb (tools/reverb_analysis.py) and fit its D-series reverb to it.

You need: the unit's MIX OUT (L and R) into an audio interface, a MIDI output to the unit's MIDI IN, and a program
that plays a MIDI file while recording (a DAW), or a MIDI file player and a recorder started together.

1. Prepare the unit.
   D-110: unit number 17. The file uses part 1: it puts it on MIDI channel 1 with output Mix + reverb, loads its
   own test tone and changes the reverb settings. Note your settings first, or save a bulk dump.
   D-10/D-20: switch to performance mode, with MIDI Exclusive on, unit number 17 and the receive channel (Rx CH)
   1. The file edits the current performance patch without storing it: select a patch afterwards to get yours
   back.
2. Set up the recording: 44.1 or 48 kHz, 24-bit if possible, stereo, no effects. Turn the unit's volume up to
   about three quarters and set the input gain so that the bursts peak around -12 dBFS; they must not clip. Keep
   the gain the same for the whole recording.
3. Start recording, then play reverb-test-d110.mid (D-110) or reverb-test-d20.mid (D-10/D-20) from the start.
   It lasts about 8 minutes: three dry bursts, a burst for every reverb type and Reverb Time at Reverb Level 7,
   Medium Hall at Reverb Levels 0-7, and a short piano phrase through every type.
4. Stop when the last phrase has died away. Save the recording as a WAV file in this folder, for example
   reverb-d20.wav, without trimming or normalising it.

reverb-test-schedule.csv lists when each burst and phrase starts (both files have the same timing).

If the recording picks up hum or noise, try another cable or input: the quieter the background, the longer a
tail can be followed.
