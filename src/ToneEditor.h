#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ToneModel.h"

// The realtime tone editor shared by D110Emu's Tone tab and ToneEditor (for real units). It shows a whole tone: name,
// structures, partial switches, the four partials' parameters side by side, their envelopes as graphs that can be
// dragged, and an audition keyboard. Like a PG-10 programmer, every change goes out at once (Host::writeTone: a DT1 to
// the part's tone temporary area), so the next note plays it; with "Restrike" on, the notes being held are played
// again after a change, so it is heard straight away.
class ToneEditor {
public:
    class Host {
    public:
        virtual ~Host() = default;
        // Bytes [offset, offset + length) of the tone changed: send them to the part's tone temporary area.
        virtual void writeTone(int offset, const uint8_t* data, int length) = 0;
        // The audition keyboard, on the edited part's channel.
        virtual void noteOn(int key, int velocity) = 0;
        virtual void noteOff(int key) = 0;
        // Plays the notes the host holds again (computer keyboard, MIDI thru), after a change.
        virtual void restrike() {}
        // Notes the part plays, for the keyboard display.
        virtual void soundingNotes(std::array<bool, 128>& notes) { (void)notes; }
    };

    struct Audition {
        int key = 60;
        int velocity = 100;
        int lengthMs = 700;
        int repeatMs = 1200;
        bool repeat = false;    // Plays the audition note every repeatMs
        bool hold = false;      // Holds the audition note down
        bool restrike = true;   // Plays held notes again after a change
    };

    ToneEditor();

    void setModel(Tone::Model model);
    Tone::Model model() const { return model_; }

    // A tone read from the synth or the unit (the part changed tones, or the tone was fetched). It becomes the original
    // that Compare plays; the edit it replaces can be undone. Nothing is sent.
    void loadTone(const Tone::Data& tone);
    // Bytes changed on the unit's side (its panel, another editor): taken in, nothing is sent back.
    void takeExternal(int offset, const uint8_t* data, int length);
    // Replaces the edit and sends the whole tone (a tone from a file or the library, Init, an MT-32 timbre). With
    // `asOriginal`, Compare plays it from then on (a newly loaded tone); the tone it replaces can be undone.
    void replaceTone(Host& host, const Tone::Data& tone, bool asOriginal = true);
    // Sends the whole tone as the unit should hold it (e.g. after connecting to another unit).
    void sendAll(Host& host);
    // Sets one byte as the editor's controls do (clamped to its range, sent at once), as an undo step of its own.
    void setByte(Host& host, int offset, int value);

    const Tone::Data& tone() const { return tone_; }  // The edit, with its own partial switches
    Tone::Data sentTone() const;                      // What the unit holds: with solo or Compare applied
    const Tone::Data& original() const { return original_; }
    bool modified() const { return tone_ != original_; }
    // The tone was written to memory or saved: it becomes the original.
    void markOriginal() { original_ = tone_; }
    // Ends solo and Compare, so the unit holds the edit (before a write request).
    void settle(Host& host);
    void solo(Host& host, int partial);  // Only this partial sounds (-1: all switched on)
    int soloPartial() const { return solo_; }
    void compare(Host& host, bool on);   // Plays the original instead of the edit
    bool comparing() const { return comparing_; }
    bool undo(Host& host);
    bool redo(Host& host);
    bool canUndo() const { return !undo_.empty(); }
    void clearHistory() {
        undo_.clear();
        redo_.clear();
        stepOpen_ = false;
    }
    bool canRedo() const { return !redo_.empty(); }

    Audition& audition() { return audition_; }
    // The keys under the audition controls (d110emu's View > Keyboard hides them); the controls stay.
    void setPianoShown(bool shown) { pianoShown_ = shown; }

    // Draws the editor into the rest of the current window. `now` in seconds drives the audition's timers.
    void draw(Host& host, double now);
    // The audition's timers (repeat, note length, restrike), when the editor is not drawn.
    void update(Host& host, double now);
    // Releases every note the editor holds (keyboard, Hold, Repeat).
    void releaseNotes(Host& host);

private:
    struct EnvelopeDrag {
        int envelope = -1;  // Tone::Envelope, -1 = none
        int partial = 0;
        int point = 0;
        int startTime = 0;
        int startLevel = 0;
        float mouseX = 0.0f;
        float mouseY = 0.0f;
    };

    void beginStep();
    // Sets a byte within the current undo step (a control held down makes one step).
    void set(Host& host, int offset, int value);
    void sync(Host& host);  // Sends what differs between the unit's copy and sentTone()
    void noteChange();
    void playNote(Host& host, int key, int velocity);
    void stopNote(Host& host, int key);

    void drawHeader(Host& host);
    void drawStructure(Host& host, int which);
    void drawEnvelopes(Host& host);
    void drawEnvelope(Host& host, Tone::Envelope envelope, float width, float height);
    void drawGrid(Host& host, float height);
    void drawPartialHeader(Host& host, int partial);
    void drawCell(Host& host, int partial, const Tone::Param& param);
    void drawWavePopup(Host& host, int partial);
    void drawAudition(Host& host, double now);

    Tone::Model model_ = Tone::Model::D110;
    Tone::Data tone_;
    Tone::Data original_;
    Tone::Data unit_;  // What the unit holds, as far as the editor knows
    std::vector<Tone::Data> undo_;
    std::vector<Tone::Data> redo_;
    bool stepOpen_ = false;  // Changes join one undo step while a control is held
    int solo_ = -1;
    bool comparing_ = false;
    int focus_ = 0;  // Partial drawn on top in the envelope graphs
    std::array<uint8_t, Tone::kPartialSize> clipboard_{};
    bool clipboardFull_ = false;
    std::string waveFilter_;
    EnvelopeDrag drag_;

    Audition audition_;
    bool pianoShown_ = true;
    double now_ = 0.0;
    std::array<int, 128> held_{};  // Velocity of notes the editor holds, 0 = not held
    int mouseNote_ = -1;
    int timedNote_ = -1;           // Play button or Repeat: released at timedOff_
    double timedOff_ = 0.0;
    double nextRepeat_ = 0.0;
    int holdKey_ = -1;             // Note held by Hold
    double lastChange_ = -1.0;
    double lastRestrike_ = -1.0;
    bool restrikePending_ = false;
};
