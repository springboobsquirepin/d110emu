#include "TuiApp.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>

#include "Lcd.h"
#include "Platform.h"
#include "TuiMenus.h"

using namespace UnitText;
using namespace Tui::Ansi;
using Tui::Color;
using Tui::Key;
using Tui::Screen;
using Tui::Style;
using Tui::findByName;
using Tui::lowerText;
using Tui::wrapText;

namespace {

constexpr double kAudioCheckSeconds = 3.0;   // A stopped audio device is tried again when the devices changed
constexpr int kMinColumns = 60;
constexpr int kMinRows = 16;
constexpr float kMeterFloorDb = 48.0f;  // The output meter shows -48 to 0 dB
constexpr int kInfoWidth = Tui::Menus::kInfoWidth;

std::string trimmed(std::string text) {
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

// The ANSI colour (of xterm's 16; with `normalOnly`, of the first 8) nearest to an RGB colour.
int nearestAnsi(const int rgb[3], bool normalOnly) {
    static const int palette[16][3] = {
        {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0}, {0, 0, 238},   {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
        {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0}, {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
    };
    int best = 0;
    long bestDistance = LONG_MAX;
    for (int i = 0; i < (normalOnly ? 8 : 16); i++) {
        long distance = 0;
        for (int c = 0; c < 3; c++) distance += long(palette[i][c] - rgb[c]) * long(palette[i][c] - rgb[c]);
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

bool parseHexColour(const std::string& text, int rgb[3]) {
    const std::string hex = !text.empty() && text[0] == '#' ? text.substr(1) : text;
    if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
    rgb[0] = int((value >> 16) & 0xFF);
    rgb[1] = int((value >> 8) & 0xFF);
    rgb[2] = int(value & 0xFF);
    return true;
}

std::string percentText(float value) {
    return std::to_string(int(std::lround(value * 100.0f))) + "%";
}

std::string decibelText(float peak) {
    if (peak < 1e-5f) return "  -inf";
    char text[16];
    std::snprintf(text, sizeof(text), "%+6.1f", 20.0 * std::log10(double(peak)));
    return text;
}

std::string rateText(int rate) {
    return rate == 0 ? std::string("Device default") : std::to_string(rate) + " Hz";
}

std::string onOff(bool on) {
    return on ? "On" : "Off";
}

bool isMidiOrSysexFile(const std::filesystem::path& path) {
    const std::string extension = lowerText(Platform::toUtf8(path.extension()));
    return extension == ".mid" || extension == ".midi" || extension == ".kar" || extension == ".smf" || extension == ".syx";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Setup

void TuiApp::setCommandLine(const std::string& romFolder, const std::string& audioDevice, const std::vector<std::string>& midiInputs) {
    commandRomFolder_ = romFolder;
    commandAudioDevice_ = audioDevice;
    commandMidiInputs_ = midiInputs;
}

double TuiApp::now() const {
    return clock_ >= 0.0 ? clock_ : AppCore::now();
}

void TuiApp::loadFrontEndSettings() {
    // The command line's choices, as if made in the menus: the synth and the audio start with them, and they are kept.
    if (!commandRomFolder_.empty()) romFolder_ = Platform::fromUtf8(commandRomFolder_);
    if (!commandAudioDevice_.empty()) {
        if (lowerText(commandAudioDevice_) == "default") {
            audioDevice_.clear();
        } else {
            const std::string found = findByName(audio_->listDevices(), commandAudioDevice_);
            if (found.empty()) addLog("Audio: no device named " + commandAudioDevice_ + "; the system default plays");
            audioDevice_ = found;
        }
    }
    if (!commandMidiInputs_.empty()) {
        const std::vector<std::string> ports = midi_->listPorts();
        for (const std::string& wanted : commandMidiInputs_) {
            const std::string found = findByName(ports, wanted);
            if (found.empty()) {
                // Plugged in later: tick() opens it when it appears, and keeps it then.
                addLog("MIDI: no input named " + wanted + " yet; it opens when it appears");
                pendingMidiInputs_.push_back(wanted);
            } else if (std::find(enabledMidiPorts_.begin(), enabledMidiPorts_.end(), found) == enabledMidiPorts_.end()) {
                enabledMidiPorts_.push_back(found);
            }
        }
    }
    setUpLcdStyles();
}

void TuiApp::onLogAdded(const std::string& line) {
    if (tuiOptions_.headless) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    } else if (echo_) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

bool TuiApp::takeRedrawRequest() {
    const bool redraw = redraw_;
    redraw_ = false;
    return redraw;
}

std::string TuiApp::summary() const {
    std::string text = "Synth: ";
    text += status_.open ? status_.controlRomName + " + " + status_.pcmRomName : "not running (" + synthError_ + ")";
    text += "\nROMs: " + Platform::toUtf8(romFolder_);
    if (!options_.enableAudio) {
        text += "\nAudio: off";
    } else if (audio_->isRunning()) {
        text += "\nAudio: " + audio_->backendName() + ", " + audio_->deviceName() + ", " + std::to_string(audio_->sampleRate()) + " Hz, " +
                std::to_string(audio_->periodFrames()) + "-frame period" + (surround_ ? ", 7.1 surround" : "");
    } else {
        text += "\nAudio: stopped" + (audioError_.empty() ? std::string() : " (" + audioError_ + ")");
    }
    const std::string midiError = midi_->systemError();
    text += "\nMIDI: " + (midiError.empty() ? midiInputsText() : midiError);
    if (!midi_->ownPortName().empty()) text += "\nMIDI: programs can send to " + midi_->ownPortName();
    if (!options_.settingsFile.empty()) text += "\nSettings: " + Platform::toUtf8(options_.settingsFile);
    return text;
}

Style TuiApp::style(int foreground, uint8_t attributes) const {
    return Tui::ansiStyle(foreground, attributes);
}

void TuiApp::setUpLcdStyles() {
    // The standalone's LCD colours (View > LCD colours): a preset, or the user's own.
    const bool custom = settings_.getString("lcd_scheme") == "custom";
    const Lcd::Colors preset = Lcd::schemeColors(custom ? 0 : std::clamp(settings_.getInt("lcd_scheme", 0), 0, Lcd::schemeCount() - 1));
    auto unpack = [](uint32_t packed, int rgb[3]) {  // IM_COL32: red in the lowest byte
        rgb[0] = int(packed & 0xFF);
        rgb[1] = int((packed >> 8) & 0xFF);
        rgb[2] = int((packed >> 16) & 0xFF);
    };
    int glass[3];
    int lit[3];
    unpack(preset.background, glass);
    unpack(preset.dotOn, lit);
    if (custom) {
        parseHexColour(settings_.getString("lcd_custom_glass"), glass);
        parseHexColour(settings_.getString("lcd_custom_dot_on"), lit);
    }
    // With 16 colours, the nearest; the Linux console has only the first 8 as backgrounds.
    const bool consoleBackground = tuiOptions_.capabilities.boldBright;
    const int glassAnsi = nearestAnsi(glass, consoleBackground);
    int litAnsi = nearestAnsi(lit, false);
    if ((litAnsi & 7) == (glassAnsi & 7)) litAnsi = glassAnsi == kBlack || glassAnsi == kBlue ? kBrightWhite : kBlack;
    lcd_ = Style();
    lcd_.bg = Color::rgb(glass[0], glass[1], glass[2], glassAnsi);
    lcd_.fg = Color::rgb(lit[0], lit[1], lit[2], litAnsi);
    lcdInverted_ = Style();
    lcdInverted_.bg = lcd_.fg;
    lcdInverted_.fg = lcd_.bg;
    if (tuiOptions_.capabilities.colors == Tui::Capabilities::Colors::None) {
        lcd_.attributes = Tui::Reverse;  // The glass in reverse video, inverted characters in normal
        lcdInverted_.attributes = 0;
    }
    lcdBezel_ = style(kGrey);
}

// ---------------------------------------------------------------------------------------------
// Frames

void TuiApp::tick() {
    const bool firstTick = lastTick_ < 0.0;
    update();
    const double time = now();
    const double elapsed = firstTick ? 0.0 : std::clamp(time - lastTick_, 0.0, 1.0);
    lastTick_ = time;

    // The parts' meters: the loudest sounding note's velocity, falling when the notes end.
    for (int part = 0; part < kMaxPartCount; part++) {
        const PartStatus& status = status_.parts[part];
        float target = 0.0f;
        if (status.active) {
            for (uint32_t i = 0; i < std::min<uint32_t>(status.noteCount, kMaxPartials); i++) target = std::max(target, status.velocities[i] / 127.0f);
        }
        partLevel_[size_t(part)] = std::max(target, partLevel_[size_t(part)] - float(elapsed) * 1.5f);
    }
    // The output: its peaks, falling 24 dB a second.
    float left = 0.0f;
    float right = 0.0f;
    takeOutputPeaks(left, right);
    const float fall = float(std::pow(10.0, -24.0 * elapsed / 20.0));
    outputLeft_ = std::max(left, outputLeft_ * fall);
    outputRight_ = std::max(right, outputRight_ * fall);
    if (left >= 0.999f || right >= 0.999f) clipUntil_ = time + 1.5;

    for (auto note = testNotes_.begin(); note != testNotes_.end();) {
        if (time < note->offAt) {
            ++note;
            continue;
        }
        for (uint8_t key : note->keys) sendShort(uint8_t(0x80 | note->channel), key, 0);
        note = testNotes_.erase(note);
    }

    // MIDI inputs named on the command line that were not there at the start open once they appear.
    for (auto wanted = pendingMidiInputs_.begin(); wanted != pendingMidiInputs_.end();) {
        const std::string found = findByName(midiPorts_, *wanted);
        if (found.empty()) {
            ++wanted;
            continue;
        }
        setMidiPortEnabled(found, true);
        wanted = pendingMidiInputs_.erase(wanted);
    }
    // MIDI activity (AppCore::update() opens the inputs that came).
    if (options_.enableMidiInput) {
        for (const std::string& name : midi_->openPorts()) {
            const uint32_t count = midi_->messageCount(name);
            if (count != midiCounts_[name]) {
                midiCounts_[name] = count;
                midiActivity_[name] = time;
            }
        }
    }
    if (status_.midiEvents != midiEvents_) {
        midiEvents_ = status_.midiEvents;
        midiEventTime_ = time;
    }

    // The audio device: when it stopped (unplugged, or it never opened), it is tried again once the devices change.
    if (options_.enableAudio) {
        if (firstTick) {
            audioDeviceList_ = audioDevices_;
            nextAudioCheck_ = time + kAudioCheckSeconds;
        } else if (!audio_->isRunning() && time >= nextAudioCheck_) {
            nextAudioCheck_ = time + kAudioCheckSeconds;
            std::vector<std::string> devices = audio_->listDevices();
            if (devices != audioDeviceList_) {
                audioDeviceList_ = devices;
                audioDevices_ = devices;
                addLog("Audio: the devices changed; trying again");
                startAudioAndSynth();
            }
        }
    }
}

void TuiApp::draw(Screen& screen) {
    screen.clear();
    if (screen.columns() < kMinColumns || screen.rows() < kMinRows) {
        const std::string text = "D110Emu needs a terminal of at least " + std::to_string(kMinColumns) + " x " + std::to_string(kMinRows);
        screen.text(0, screen.rows() / 2, text, Style(), screen.columns());
        return;
    }
    if (showLog_) {
        drawLogView(screen);
    } else {
        drawStatus(screen);
    }
    if (showHelp_) drawHelp(screen);
    menus_.draw(screen);
}

void TuiApp::drawTitle(Screen& screen, const std::string& title) {
    const int width = screen.columns();
    const Style bar = style(-1, Tui::Reverse);
    screen.fill(0, 0, width, 1, U' ', bar);
    int x = 1 + screen.text(1, 0, "D110Emu", style(-1, Tui::Reverse | Tui::Bold)) + 2;
    std::vector<std::string> flags;
    if (status_.mt32Translation) flags.push_back("MT-32 translation");
    if (status_.performanceMode) flags.push_back("Performance");
    if (status_.partCount > uint32_t(kBasePartCount)) flags.push_back("16 parts");
    if (options_.enableAudio) {
        if (audio_->isRunning()) {
            flags.push_back(audio_->backendName() + " " + std::to_string(audio_->sampleRate()) + " Hz" + (surround_ ? " 7.1" : ""));
        } else {
            flags.push_back("no audio");
        }
    }
    std::string right;
    for (const std::string& flag : flags) right += (right.empty() ? "" : "  ") + flag;
    const int rightWidth = Tui::textWidth(right);
    const int titleWidth = std::max(0, width - x - rightWidth - 3);
    screen.text(x, 0, title, bar, titleWidth);
    if (rightWidth + x + 2 < width) screen.text(width - rightWidth - 1, 0, right, bar);
}

void TuiApp::drawStatus(Screen& screen) {
    const int width = screen.columns();
    const int height = screen.rows();
    drawTitle(screen, status_.open ? status_.controlRomName : std::string("no synth"));
    drawLcd(screen, 1, 1);
    drawInfo(screen, 23, 1, width - 24);

    // Under the unit: the parts, then the output meter, the partials and the log, as the rows allow (the parts scroll
    // when they cannot all show), and the keys at the bottom.
    const int partCount = int(partOrder().size());
    const int top = 6;
    const int space = height - 1 - top;
    bool gap = true;
    bool partials = true;
    auto fixedRows = [&] { return 1 + (partials ? 1 : 0) + (gap ? 1 : 0); };  // The output meter, the partials, a gap
    if (1 + partCount + fixedRows() + 1 > space) gap = false;
    if (1 + partCount + fixedRows() + 1 > space) partials = false;
    const int tableRows = std::min(1 + partCount, std::max(3, space - fixedRows() - 1));
    const int logRows = std::max(1, space - fixedRows() - tableRows);
    int y = top;
    drawParts(screen, y, tableRows);
    y += tableRows + (gap ? 1 : 0);
    drawOutputMeter(screen, y++);
    if (partials) drawPartials(screen, y++);
    drawLogLines(screen, y, logRows);

    std::vector<std::pair<std::string, std::string>> hints = {
        {"Q", "Quit"}, {"?", "Help"}, {"O", "Options"}, {"T", "MT-32"}, {"X", "Reset MIDI"}, {"R", "ROM Play"}, {"F", "Files"},
        {"Tab", "Log"},
    };
    if (status_.player.state != MidiPlayer::State::Empty) hints.insert(hints.begin() + 7, {"Space", status_.player.state == MidiPlayer::State::Playing ? "Pause" : "Play"});
    Tui::drawHints(screen, hints);
}

void TuiApp::drawLcd(Screen& screen, int x, int y) {
    const std::string text = lcdText();
    // The bezel, and the glass with a column of it either side of the characters.
    screen.box(x, y, 20, 4, lcdBezel_);
    screen.fill(x + 1, y + 1, 18, 2, U' ', lcd_);
    for (int row = 0; row < Lcd::kRows; row++) {
        for (int column = 0; column < Lcd::kColumns; column++) {
            const size_t index = size_t(row * Lcd::kColumns + column);
            const uint8_t raw = index < text.size() ? uint8_t(text[index]) : uint8_t(' ');
            const uint8_t code = raw & 0x7F;
            // The LCD's font (Lcd.cpp's): ASCII with the yen sign at 5CH, as the units' Japanese LCDs have it, and a full
            // block at 7FH; characters with bit 7 set are shown inverted.
            const char32_t ch = code == 0x7F ? U'█' : code == 0x5C ? U'¥' : code < 0x20 ? U' ' : char32_t(code);
            screen.set(x + 2 + column, y + 1 + row, ch, (raw & 0x80) != 0 ? lcdInverted_ : lcd_);
        }
    }
}

void TuiApp::drawInfo(Screen& screen, int x, int y, int width) {
    const Style label = style(-1, Tui::Dim);
    const Style value;
    const Style problem = style(kRed, Tui::Bold);
    const int right = x + width;
    // Writes `text` at `column`, within the info column; returns the column after it.
    auto put = [&](int column, int row, const std::string& text, const Style& textStyle) {
        if (column >= right) return column;
        return column + screen.text(column, y + row, text, textStyle, right - column);
    };
    const double time = now();

    // Reverb, as the unit names its types.
    int column = put(x, 0, "Reverb  ", label);
    if (!status_.open) {
        put(column, 0, "-", value);
    } else {
        const int mode = std::min<int>(status_.reverbMode, status_.d110 ? 8 : 3);
        std::string reverb = status_.d110 && mode == 8 ? std::string("Off")
                                                       : std::string(status_.d110 ? kD110ReverbNames[mode] : kReverbModeNames[mode]) +
                                                             ", time " + std::to_string(status_.reverbTime + 1) + ", level " +
                                                             std::to_string(status_.reverbLevel);
        column = put(column, 0, reverb, value);
        if (!engineOptions_.reverbEnabled) put(column, 0, "  (switched off)", style(kYellow));
    }

    // Tune, and the patch (D-110), the performance patch (performance mode) or the master volume (MT-32).
    column = put(x, 1, "Tune    ", label);
    if (status_.open) {
        std::string tune = masterTuneText(status_.masterTune);
        if (status_.gsMasterTune != 0) {
            char text[32];
            std::snprintf(text, sizeof(text), " %+.1f cents", status_.gsMasterTune / 10.0);
            tune += text;
        }
        column = put(column, 1, tune, value) + 3;
        if (status_.performanceMode) {
            const std::string name(reinterpret_cast<const char*>(&status_.performanceTemp[0x15]), 16);
            column = put(column, 1, "Perf ", label);
            column = put(column, 1, performanceCode(status_.currentPerformance) + " " + trimmed(name), value);
            put(column, 1, " ch " + std::to_string(status_.performanceChannel + 1), label);
        } else if (status_.d110) {
            column = put(column, 1, "Patch ", label);
            put(column, 1, patchCode(status_.currentPatch) + " " + trimmed(status_.patchName), value);
        } else {
            column = put(column, 1, "Volume ", label);
            put(column, 1, std::to_string(status_.masterVolume), value);
        }
    }

    // The MIDI file player.
    column = put(x, 2, "Player  ", label);
    const PlayerStatus& player = status_.player;
    if (player.state == MidiPlayer::State::Empty) {
        put(column, 2, "F: a MIDI or SysEx file   R: ROM Play", label);
    } else {
        const char* state = player.state == MidiPlayer::State::Playing ? "Playing"
                          : player.state == MidiPlayer::State::Paused  ? "Paused"
                                                                       : "Stopped";
        column = put(column, 2, state, player.state == MidiPlayer::State::Playing ? style(kGreen, Tui::Bold) : value);
        column = put(column + 1, 2, formatTime(player.position) + "/" + formatTime(player.duration), label);
        if (player.loop) column = put(column + 1, 2, "loop", label);
        column = put(column + 1, 2, player.name, value);
        if (!player.queuedName.empty()) put(column + 2, 2, "next: " + player.queuedName, style(kYellow));
    }

    // MIDI input: the open inputs with their activity lights, what went wrong, where other programs can send.
    column = put(x, 3, "MIDI in ", label);
    const std::string systemError = midi_->systemError();
    const std::string& error = !systemError.empty() ? systemError : midiError_;
    if (!error.empty()) {
        // What went wrong, on the row under this one too if it needs it.
        const std::vector<std::string> lines = wrapText(error, right - column);
        for (size_t i = 0; i < lines.size() && i < 2; i++) put(column, 3 + int(i), lines[i], problem);
    } else {
        const std::vector<std::string> open = midi_->openPorts();
        for (const std::string& name : open) {
            const auto activity = midiActivity_.find(name);
            const bool lit = activity != midiActivity_.end() && time - activity->second < 0.15;
            column = put(column, 3, "●", lit ? style(kBrightGreen, Tui::Bold) : style(kGrey));
            column = put(column + 1, 3, name, value) + 2;
        }
        if (open.empty()) column = put(column, 3, "none chosen (O)", label) + 2;
        const std::string own = midi_->ownPortName();
        if (!own.empty()) {
            column = put(column, 3, "●", time - midiEventTime_ < 0.15 && midiEventTime_ >= 0.0 ? style(kBrightGreen, Tui::Bold) : style(kGrey));
            put(column + 1, 3, "programs: " + own, label);
        }
    }
}

void TuiApp::drawBar(Screen& screen, int x, int y, int width, float level, const Style& filled, const Style& empty) {
    // Eighths of a cell with Unicode's blocks, halves on the Linux console, whole cells in ASCII.
    static const char32_t kEighths[8] = {U' ', U'▏', U'▎', U'▍', U'▌', U'▋', U'▊', U'▉'};
    const Tui::Capabilities::Glyphs glyphs = tuiOptions_.capabilities.glyphs;
    const int steps = glyphs == Tui::Capabilities::Glyphs::Unicode ? 8 : glyphs == Tui::Capabilities::Glyphs::Console ? 2 : 1;
    const int units = int(std::lround(std::clamp(level, 0.0f, 1.0f) * float(width * steps)));
    for (int i = 0; i < width; i++) {
        const int inCell = std::clamp(units - i * steps, 0, steps);
        if (inCell == steps) {
            screen.set(x + i, y, U'█', filled);
        } else if (inCell > 0) {
            screen.set(x + i, y, steps == 8 ? kEighths[inCell] : U'▌', filled);
        } else {
            screen.set(x + i, y, U'·', empty);
        }
    }
}

int TuiApp::drawParts(Screen& screen, int y, int rows) {
    const int width = screen.columns();
    const std::vector<int> order = partOrder();
    const bool finePan = engineOptions_.nicePanning && status_.d110;
    const bool showVolume = width >= 78;
    const bool showExpression = width >= 92;
    const bool showMeter = width >= 70;
    // Columns: the selection mark and part (0-2), mute and solo (4-5), channel (7-9), timbre (11-24), level (26-28),
    // pan (30-32), output (34-40), then what the width allows.
    const int channelX = 7, timbreX = 11, levelX = 26, panX = 30, outputX = 34;
    int next = 42;
    const int volumeX = showVolume ? next : -1;
    if (showVolume) next += 4;
    const int expressionX = showExpression ? next : -1;
    if (showExpression) next += 4;
    const int partialsX = next;
    next += 6;
    const int meterWidth = showMeter ? std::clamp((width - next) / 3, 6, 14) : 0;
    const int meterX = showMeter ? next : -1;
    if (showMeter) next += meterWidth + 1;
    const int notesX = next;
    const int notesWidth = width - notesX - 1;

    const Style heading = style(-1, Tui::Dim);
    auto rightAligned = [&](int x, int row, int columnWidth, const std::string& text, const Style& textStyle) {
        const int textLength = std::min(Tui::textWidth(text), columnWidth);
        screen.text(x + columnWidth - textLength, row, text, textStyle, columnWidth);
    };
    screen.text(0, y, "Part", heading);
    rightAligned(channelX, y, 3, "Ch", heading);
    screen.text(timbreX, y, status_.d110 ? "Timbre" : "Patch", heading);
    rightAligned(levelX, y, 3, "Lvl", heading);
    screen.text(panX, y, "Pan", heading);
    screen.text(outputX, y, "Output", heading);
    if (showVolume) rightAligned(volumeX, y, 3, "Vol", heading);
    if (showExpression) rightAligned(expressionX, y, 3, "Exp", heading);
    rightAligned(partialsX, y, 5, "Ptl", heading);
    if (showMeter) screen.text(meterX, y, "Level", heading);
    if (notesWidth > 4) screen.text(notesX, y, "Notes", heading);

    // The rows: the selected part kept in view.
    const int visible = std::max(1, rows - 1);
    const int count = int(order.size());
    const int selected = int(std::find(order.begin(), order.end(), lcdPart_) - order.begin());
    if (selected < count) {
        if (selected < partsScroll_) partsScroll_ = selected;
        if (selected >= partsScroll_ + visible) partsScroll_ = selected - visible + 1;
    }
    partsScroll_ = std::clamp(partsScroll_, 0, std::max(0, count - visible));
    if (partsScroll_ > 0) screen.set(width - 1, y + 1, U'▲', heading);
    if (partsScroll_ + visible < count) screen.set(width - 1, y + visible, U'▼', heading);

    const bool anySolo = std::find(partSolo_.begin(), partSolo_.end(), true) != partSolo_.end();
    for (int row = 0; row < visible && partsScroll_ + row < count; row++) {
        const int part = order[size_t(partsScroll_ + row)];
        const PartStatus& status = status_.parts[part];
        const int line = y + 1 + row;
        const bool rhythm = part == kRhythmPart;
        const bool silenced = partMuted_[size_t(part)] || (anySolo && !partSolo_[size_t(part)]);
        const Style base = silenced ? style(-1, Tui::Dim) : Style();

        if (part == lcdPart_) screen.set(0, line, U'▸', style(-1, Tui::Bold));
        Style labelStyle = status.active ? style(kBrightGreen, Tui::Bold) : base;
        if (part == lcdPart_) labelStyle.attributes |= Tui::Reverse;
        rightAligned(1, line, 2, partLabel(part), labelStyle);
        if (partMuted_[size_t(part)]) screen.set(4, line, U'M', style(kRed, Tui::Bold));
        if (partSolo_[size_t(part)]) screen.set(5, line, U'S', style(kYellow, Tui::Bold));
        rightAligned(channelX, line, 3, status.channel < 16 ? std::to_string(status.channel + 1) : std::string("Off"), base);

        std::string timbre = "Rhythm";
        if (!rhythm) {
            std::string code;
            if (!status_.d110) {
                code = status.program != 0xFF ? std::to_string(status.program + 1) : std::string("--");
            } else {
                code = status.program != 0xFF ? timbreCode(partTimbre(status)).substr(2) : std::string(status.tone);
            }
            timbre = code + " " + trimmed(status.name);
        }
        screen.text(timbreX, line, timbre, base, 14);
        rightAligned(levelX, line, 3, std::to_string(status.temp[TimbreTemp::OutputLevel]), base);
        if (!rhythm) {
            std::string pan;
            if (finePan) {
                pan = status.finePan == 0 ? std::string("0") : (status.finePan > 0 ? "+" : "") + std::to_string(status.finePan);
            } else {
                const int panpot = status.temp[TimbreTemp::Panpot];
                pan = panLabel(status_.d110 ? panpot : 14 - panpot);  // The MT-32 stores pan the other way round
            }
            screen.text(panX, line, pan, base, 3);
        }
        const int assign = status.temp[TimbreTemp::OutputAssign];
        std::string output;
        if (status_.d110) {
            output = rhythm ? "Per key" : outputName(assign, true);
        } else if (!rhythm) {
            output = kMT32OutputNames[std::min(assign, 1)];
        }
        screen.text(outputX, line, output, base, 7);
        if (showVolume) rightAligned(volumeX, line, 3, std::to_string(status.midiVolume), base);
        if (showExpression) rightAligned(expressionX, line, 3, std::to_string(status.expression), base);
        const bool overReserve = status.activePartials > status.reservedPartials;
        rightAligned(partialsX, line, 5, std::to_string(status.activePartials) + "/" + std::to_string(status.reservedPartials),
                     overReserve ? style(kYellow) : base);
        if (showMeter) drawBar(screen, meterX, line, meterWidth, partLevel_[size_t(part)], silenced ? style(kGrey) : style(kGreen), style(kGrey));
        if (notesWidth > 4) {
            std::vector<uint8_t> keys(status.keys, status.keys + std::min<uint32_t>(status.noteCount, kMaxPartials));
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            std::string notes;
            size_t shown = 0;
            for (; shown < keys.size(); shown++) {
                const std::string name = noteName(keys[shown]);
                const std::string more = " +" + std::to_string(keys.size() - shown - 1);
                if (Tui::textWidth(notes) + 1 + int(name.size()) + (shown + 1 < keys.size() ? int(more.size()) : 0) > notesWidth) break;
                notes += (notes.empty() ? "" : " ") + name;
            }
            if (shown < keys.size()) notes += (notes.empty() ? "+" : " +") + std::to_string(keys.size() - shown);
            screen.text(notesX, line, notes, base, notesWidth);
        }
    }
    return rows;
}

void TuiApp::drawOutputMeter(Screen& screen, int y) {
    const int width = screen.columns();
    const Style label = style(-1, Tui::Dim);
    const double time = now();
    screen.text(0, y, "Output", label);
    const int meterWidth = std::clamp((width - 8 - 2 * 10 - 22) / 2, 6, 30);
    int x = 8;
    for (int side = 0; side < 2; side++) {
        const float peak = side == 0 ? outputLeft_ : outputRight_;
        screen.text(x, y, side == 0 ? "L" : "R", label);
        const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
        const float level = std::clamp((db + kMeterFloorDb) / kMeterFloorDb, 0.0f, 1.0f);
        drawBar(screen, x + 2, y, meterWidth, level, style(kGreen), style(kGrey));
        // -12 dB and up yellow, -3 dB and up red.
        for (int i = 0; i < meterWidth; i++) {
            const float position = float(i + 1) / float(meterWidth);
            if (screen.at(x + 2 + i, y).ch == U'·') continue;
            if (position > (kMeterFloorDb - 3.0f) / kMeterFloorDb) {
                screen.setStyle(x + 2 + i, y, style(kRed));
            } else if (position > (kMeterFloorDb - 12.0f) / kMeterFloorDb) {
                screen.setStyle(x + 2 + i, y, style(kYellow));
            }
        }
        screen.text(x + 3 + meterWidth, y, decibelText(peak), label);
        x += meterWidth + 11;
    }
    x = screen.text(x, y, "Gain ", label) + x;
    x += screen.text(x, y, percentText(engineOptions_.outputGain), Style());
    if (time < clipUntil_) screen.text(x + 2, y, "CLIP", style(kRed, Tui::Bold | Tui::Reverse));
}

void TuiApp::drawPartials(Screen& screen, int y) {
    const int width = screen.columns();
    const Style label = style(-1, Tui::Dim);
    int active = 0;
    for (uint32_t i = 0; i < status_.partialCount && i < status_.partialStates.size(); i++) {
        if (status_.partialStates[i] != MT32Emu::PartialState_INACTIVE) active++;
    }
    screen.text(0, y, "Partials", label);
    const std::string count = std::to_string(active) + "/" + std::to_string(status_.partialCount);
    screen.text(9, y, count, Style());
    // Each partial (or, with many, the busiest of a group of them): attack green, sustain yellow, release red.
    const int x = 19;
    const int cells = std::min<int>(int(status_.partialCount), width - x - 1);
    if (cells <= 0) return;
    const int group = (int(status_.partialCount) + cells - 1) / cells;
    const Style states[4] = {style(kGrey), style(kGreen), style(kYellow), style(kRed)};
    for (int cell = 0; cell * group < int(status_.partialCount) && cell < cells; cell++) {
        int state = 0;
        for (int i = cell * group; i < (cell + 1) * group && i < int(status_.partialStates.size()); i++) {
            state = std::max(state, int(status_.partialStates[size_t(i)]));
        }
        state = std::clamp(state, 0, 3);
        screen.set(x + cell, y, state == 0 ? U'·' : U'■', states[state]);
    }
}

void TuiApp::drawLogLines(Screen& screen, int y, int rows) {
    // The newest log lines under one another, then what keeps the unit from sounding or hearing MIDI, if anything.
    const int width = screen.columns();
    std::string problem = setupProblem(" Press O for the options.");
    if (problem.empty() && options_.enableMidiInput && !midi_->systemError().empty()) problem = "MIDI input: " + midi_->systemError();
    const int logRows = std::min(int(log_.size()), rows - (problem.empty() ? 0 : 1));
    const Style dim = style(-1, Tui::Dim);
    for (int row = 0; row < logRows; row++) screen.text(0, y + row, log_[log_.size() - size_t(logRows - row)], dim, width);
    if (!problem.empty() && rows > 0) screen.text(0, y + std::max(logRows, 0), problem, style(kRed, Tui::Bold), width);
}

void TuiApp::drawLogView(Screen& screen) {
    drawTitle(screen, "Log");
    const int rows = screen.rows() - 2;
    // The lines as they show, long ones wrapped (with an indent after the first).
    std::vector<std::string> lines;
    for (const std::string& entry : log_) {
        const std::vector<std::string> wrapped = wrapText(entry, screen.columns() - 2);
        for (size_t i = 0; i < wrapped.size(); i++) lines.push_back((i > 0 ? "  " : "") + wrapped[i]);
    }
    const int count = int(lines.size());
    logScroll_ = std::clamp(logScroll_, 0, std::max(0, count - rows));
    const int first = std::max(0, count - rows - logScroll_);
    for (int row = 0; row < rows && first + row < count; row++) {
        screen.text(0, 1 + row, lines[size_t(first + row)], Style(), screen.columns());
    }
    if (log_.empty()) screen.text(0, 1, "Nothing yet.", style(-1, Tui::Dim));
    Tui::drawHints(screen, {{"Tab", "The unit"}, {"↑↓ PgUp PgDn", "Scroll"}, {"End", "Newest"}, {"Q", "Quit"}});
}

void TuiApp::drawHelp(Screen& screen) {
    static const char* const kKeys[][2] = {
        {"↑ ↓", "Choose a part (the display shows it)"},
        {"← →", "The display's part, as the panel's buttons change it"},
        {"", "(performance mode: the patch, or its upper and lower tones)"},
        {"Enter", "A test note on the part (rhythm: bass drum and hi-hat)"},
        {"M  S  U", "Mute or solo the part; U: none muted or soloed"},
        {"P", "The patch on the display (D-110 ROMs)"},
        {"Esc", "The display back from a SysEx message"},
        {"T", "MT-32 translation on or off"},
        {"X", "Reset MIDI: sound off, controllers, levels and pans reset"},
        {"+  -", "Output gain"},
        {"R", "ROM Play: the demo songs in the control ROM"},
        {"F", "Play a MIDI file or load a SysEx file"},
        {"Space", "Play or pause the MIDI file; Backspace stops, L loops"},
        {"O", "Options: audio, MIDI inputs, ROMs, sound, system"},
        {"Tab", "The log"},
        {"Q", "Quit (the memory is kept for the next start)"},
    };
    const int count = int(sizeof(kKeys) / sizeof(kKeys[0]));
    const int width = std::min(screen.columns() - 4, 78);
    const int height = std::min(screen.rows() - 2, count + 3);
    const int x = (screen.columns() - width) / 2;
    const int y = std::max(1, (screen.rows() - height) / 2);
    screen.fill(x, y, width, height, U' ', Style());
    screen.box(x, y, width, height, Style(), "Keys");
    for (int i = 0; i < count && i < height - 3; i++) {
        screen.text(x + 2, y + 2 + i, kKeys[i][0], style(-1, Tui::Bold), 12);
        screen.text(x + 15, y + 2 + i, kKeys[i][1], Style(), width - 17);
    }
    screen.text(x + width - 20, y + height - 1, " Any key closes ", style(-1, Tui::Dim));
}

// ---------------------------------------------------------------------------------------------
// Keys

void TuiApp::onKey(const Key& key) {
    if (key.code == Key::Char && key.ctrl && key.ch == U'c') {
        quit_ = true;
        return;
    }
    if (key.code == Key::Char && key.ctrl && key.ch == U'l') {
        redraw_ = true;
        return;
    }
    if (menus_.active()) {
        menus_.onKey(key);
    } else if (showHelp_) {
        showHelp_ = false;
    } else {
        onStatusKey(key);
    }
}

void TuiApp::onStatusKey(const Key& key) {
    if (key.is(U'q') || key.code == Key::F10) {
        quit_ = true;
        return;
    }
    if (key.code == Key::Tab || key.code == Key::BackTab) {
        showLog_ = !showLog_;
        logScroll_ = 0;
        return;
    }
    if (showLog_) {
        const int page = 10;
        switch (key.code) {
        case Key::Up: logScroll_++; return;
        case Key::Down: logScroll_ = std::max(0, logScroll_ - 1); return;
        case Key::PageUp: logScroll_ += page; return;
        case Key::PageDown: logScroll_ = std::max(0, logScroll_ - page); return;
        case Key::Home: logScroll_ = int(log_.size()); return;
        case Key::End: logScroll_ = 0; return;
        case Key::Escape: showLog_ = false; return;
        default: break;
        }
    }
    switch (key.code) {
    case Key::Up: moveSelection(-1); return;
    case Key::Down: moveSelection(1); return;
    case Key::Left:
    case Key::Right:
        if (status_.performanceMode && romPlaying_ < 0) {
            lcdToneView_ = !lcdToneView_;  // The D-20's DISPLAY buttons
            lcdViewChanged();
        } else {
            const std::vector<int> order = partOrder();
            const int position = int(std::find(order.begin(), order.end(), lcdPart_) - order.begin()) % int(order.size());
            const int step = key.code == Key::Left ? -1 : 1;
            lcdPart_ = order[size_t((position + step + int(order.size())) % int(order.size()))];
            editPart_ = lcdPart_;
            lcdViewChanged();
        }
        return;
    case Key::Enter: playTestNote(); return;
    case Key::Escape: dismissLcdMessage(); return;
    case Key::Backspace:
        if (status_.player.state != MidiPlayer::State::Empty) engine_->playerStop();
        return;
    case Key::F1: showHelp_ = true; return;
    case Key::F2: openOptions(); return;
    default: break;
    }
    if (key.code != Key::Char || key.ctrl) return;
    switch (key.ch) {
    case U'?': case U'h': case U'H': showHelp_ = true; return;
    case U'o': case U'O': openOptions(); return;
    case U'n': case U'N': playTestNote(); return;
    case U'm': case U'M': toggleMute(false); return;
    case U's': case U'S': toggleMute(true); return;
    case U'u': case U'U':
        partMuted_.fill(false);
        partSolo_.fill(false);
        applyPartMutes();
        return;
    case U'p': case U'P':
        if (status_.d110) {
            lcdPatchView_ = !lcdPatchView_;
            lcdViewChanged();
        }
        return;
    case U't': case U'T':
        setMt32Mode(!mt32Mode_);
        if (!status_.d110) addLog(std::string("MT-32 translation ") + (mt32Mode_ ? "on" : "off") + ": it plays with the D-110 ROMs");
        return;
    case U'x': case U'X':
        engine_->resetMidiChannels();
        addLog("Reset MIDI: sound off, controllers reset, levels 100, pans centred");
        return;
    case U'+': case U'=':
        engineOptions_.outputGain = std::min(4.0f, std::round(engineOptions_.outputGain * 20.0f + 1.0f) / 20.0f);
        setOptions();
        return;
    case U'-': case U'_':
        engineOptions_.outputGain = std::max(0.0f, std::round(engineOptions_.outputGain * 20.0f - 1.0f) / 20.0f);
        setOptions();
        return;
    case U'r': case U'R': openRomPlay(); return;
    case U'f': case U'F': {
        std::error_code ec;
        openFiles(!lastMidiFolder_.empty() && std::filesystem::is_directory(lastMidiFolder_, ec) ? lastMidiFolder_ : std::filesystem::current_path(ec));
        return;
    }
    case U' ':
        if (status_.player.state == MidiPlayer::State::Playing) {
            engine_->playerPause();
        } else if (status_.player.state != MidiPlayer::State::Empty) {
            engine_->playerPlay();
        }
        return;
    case U'l': case U'L':
        engine_->playerSetLoop(!status_.player.loop);
        return;
    default:
        return;
    }
}

void TuiApp::moveSelection(int step) {
    const std::vector<int> order = partOrder();
    const int position = int(std::find(order.begin(), order.end(), lcdPart_) - order.begin());
    const int moved = std::clamp(position + step, 0, int(order.size()) - 1);
    if (order[size_t(moved)] == lcdPart_) return;
    lcdPart_ = order[size_t(moved)];
    editPart_ = lcdPart_;
    lcdViewChanged();
}

void TuiApp::playTestNote() {
    const PartStatus& part = status_.parts[lcdPart_];
    if (!status_.open) return;
    if (part.channel >= 16) {
        addLog("Part " + partLabel(lcdPart_) + " has no MIDI channel");
        return;
    }
    TestNote note;
    note.channel = part.channel;
    const bool rhythm = lcdPart_ == kRhythmPart;
    note.keys = rhythm ? std::vector<uint8_t>{36, 42} : std::vector<uint8_t>{60, 64, 67};  // Bass drum and closed hi-hat, C major
    for (uint8_t key : note.keys) sendShort(uint8_t(0x90 | note.channel), key, 100);
    note.offAt = now() + (rhythm ? 0.3 : 0.8);
    testNotes_.push_back(note);
}

void TuiApp::toggleMute(bool solo) {
    bool& flag = solo ? partSolo_[size_t(lcdPart_)] : partMuted_[size_t(lcdPart_)];
    flag = !flag;
    applyPartMutes();
}

// ---------------------------------------------------------------------------------------------
// Menus

void TuiApp::openOptions() {
    menus_.open("Options", [this](std::vector<MenuItem>& items) { buildOptions(items); });
}

void TuiApp::buildOptions(std::vector<MenuItem>& items) {
    auto heading = [&](const std::string& title) {
        MenuItem item;
        item.label = title;
        item.heading = true;
        items.push_back(item);
    };
    auto info = [&](const std::string& text, bool problem = false) {
        for (const std::string& line : wrapText(text, kInfoWidth)) {
            MenuItem item;
            item.label = line;
            item.info = true;
            item.problem = problem;
            items.push_back(item);
        }
    };
    auto add = [&](const std::string& label, const std::string& value, std::function<void()> activate, std::function<void(int)> adjust,
                   const std::string& help, bool enabled = true) {
        MenuItem item;
        item.label = label;
        item.value = value;
        item.activate = std::move(activate);
        item.adjust = std::move(adjust);
        item.help = help;
        item.enabled = enabled;
        items.push_back(item);
    };
    // An on/off setting of the engine's options.
    auto engineToggle = [&](const std::string& label, bool EngineOptions::* field, const std::string& help) {
        add(label, onOff(engineOptions_.*field), [this, field] {
            engineOptions_.*field = !(engineOptions_.*field);
            setOptions();
        }, [this, field](int) {
            engineOptions_.*field = !(engineOptions_.*field);
            setOptions();
        }, help);
    };
    // A step through a list of values, and the list itself on Enter.
    auto stepper = [&](const std::string& label, const std::vector<std::string>& labels, int current, std::function<void(int)> choose,
                       const std::string& help, bool enabled = true) {
        auto chooseShared = std::make_shared<std::function<void(int)>>(std::move(choose));
        const int count = int(labels.size());
        add(label, current >= 0 && current < count ? labels[size_t(current)] : std::string("-"),
            [this, label, labels, current, chooseShared] { menus_.openChoice(label, labels, current, *chooseShared); },
            [current, count, chooseShared](int step) {
                const int next = std::clamp(current + step, 0, count - 1);
                if (next != current) (*chooseShared)(next);
            },
            help, enabled);
    };

    const bool open = status_.open;
    if (options_.enableAudio) {
        heading("Audio output");
        {
            const std::string value = audioDevice_.empty() ? std::string("System default") : audioDevice_;
            add("Device", value, [this] {
                audioDevices_ = audio_->listDevices();
                std::vector<std::string> list = {"System default"};
                list.insert(list.end(), audioDevices_.begin(), audioDevices_.end());
                int chosen = 0;
                for (size_t i = 0; i < audioDevices_.size(); i++) {
                    if (audioDevices_[i] == audioDevice_) chosen = int(i) + 1;
                }
                menus_.openChoice("Audio device", list, chosen, [this, list](int index) { chooseAudioDevice(index == 0 ? std::string() : list[size_t(index)]); });
            }, nullptr, "The audio output. A device that is not there (unplugged) is taken again when it comes back.");
        }
        {
            std::vector<std::string> labels;
            int current = 0;
            for (int i = 0; i < int(std::size(kSampleRates)); i++) {
                labels.push_back(rateText(kSampleRates[i]));
                if (kSampleRates[i] == sampleRate_) current = i;
            }
            stepper("Sample rate", labels, current, [this](int index) {
                sampleRate_ = kSampleRates[index];
                startAudioAndSynth();
                saveSettings();
            }, "The synth renders at 32 kHz; its analog stage and the resampler make the output rate.");
        }
        {
            std::vector<std::string> labels;
            int current = 0;
            const double rate = double(outputRate());
            for (int i = 0; i < int(std::size(kBufferSizes)); i++) {
                char text[48];
                std::snprintf(text, sizeof(text), "%d frames (%.1f ms)", kBufferSizes[i], 1000.0 * kBufferSizes[i] / rate);
                labels.push_back(text);
                if (kBufferSizes[i] == bufferFrames_) current = i;
            }
            stepper("Buffer", labels, current, [this](int index) {
                bufferFrames_ = kBufferSizes[index];
                startAudioAndSynth();
                saveSettings();
            }, "Smaller buffers lower the latency but may crackle on a busy system.");
        }
        stepper("Channels", {"Stereo", "7.1 surround"}, surround_ ? 1 : 0, [this](int index) {
            surround_ = index == 1;
            startAudioAndSynth();
            saveSettings();
        }, "7.1 surround gives MULTI 1-6 speakers of their own: 1 and 2 at the front with the mix, 3 and 4 at the rear, 5 and 6 at "
           "the side, each alone or as stereo pairs (Multi). Stereo: MULTI 1-6 are in the mix, dry and centred, and 5 and 6 are "
           "silent with reverb on, as on the unit.");
        stepper("Multi", {"Six mono outputs", "Three stereo pairs"}, multiPairs_ ? 1 : 0, [this](int index) { setMultiPairs(index == 1); },
                "How the 7.1 speakers play MULTI 1-6. Six mono outputs: each on a speaker of its own, as the unit's mono jacks. "
                "Three stereo pairs: 1+2 at the front, 3+4 at the rear and 5+6 at the side, where a part or rhythm key on either "
                "output of a pair plays in stereo with its pan. In stereo, MULTI 1-6 play in the mix either way.");
        if (audio_->isRunning()) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s, %u Hz, %u-frame period (%.1f ms)%s", audio_->backendName().c_str(), audio_->sampleRate(),
                          audio_->periodFrames(), 1000.0 * audio_->periodFrames() / std::max(1u, audio_->sampleRate()),
                          surround_ && audio_->deviceSpeakers() != audio_->speakers() ? ", the device lacks some of the 7.1 speakers" : "");
            info(text);
        } else {
            info("Audio stopped" + (audioError_.empty() ? std::string(".") : ": " + audioError_), true);
        }
        add("Restart audio", "", [this] { startAudioAndSynth(); }, nullptr, "Opens the audio device again (and restarts the synth).");
    }

    heading("MIDI input");
    add("MIDI inputs", midiInputsText(), [this] { openMidiInputs(); }, nullptr,
        "The MIDI inputs to play from, remembered by name: an input that is unplugged opens again when it comes back.");
    engineToggle("Jitter-free timing", &EngineOptions::midiTimestamping,
                 "Plays each event exactly one audio buffer after it arrives, instead of at the start of the next buffer: one "
                 "buffer more latency, steadier rhythm.");
    engineToggle("MIDI cable speed", &EngineOptions::midiCableSpeed,
                 "Spaces messages as a MIDI cable delivers them to a real D-110 (about 0.8 ms per note). Off suits USB and "
                 "virtual ports.");
    {
        // Switched on, they are still off for music made for the units.
        std::string value = onOff(engineOptions_.midiExtensions);
        if (engineOptions_.midiExtensions && status_.open && !status_.midiExtensions) {
            value += !status_.d110 ? " (off: MT-32 ROMs)" : status_.mt32Translation ? " (off: translating)" : " (off: ROM Play)";
        }
        const auto toggle = [this] {
            engineOptions_.midiExtensions = !engineOptions_.midiExtensions;
            setOptions();
        };
        add("MIDI extensions", value, toggle, [toggle](int) { toggle(); },
            "Acts more like a GM module: channel bend ranges, filter, envelope and vibrato NRPNs, portamento, GM/GS/XG resets. "
            "Off for MT-32 music and ROM Play.");
    }

    heading("ROMs");
    add("ROM folder", Platform::toUtf8(romFolder_), [this] {
        menus_.openTextInput("ROM folder", "The folder with the ROM images (the D-110's Control and PCM ROMs):", Platform::toUtf8(romFolder_),
                      [this](const std::string& folder) { setRomFolder(folder); });
    }, nullptr, "Where the ROM images are. Without one, a \"roms\" folder is searched next to the program and above it.");
    for (int control = 1; control >= 0; control--) {
        std::vector<std::string> labels;
        std::vector<int> indexes;
        int current = -1;
        for (int i = 0; i < int(roms_.size()); i++) {
            if (roms_[size_t(i)].isControl != (control == 1)) continue;
            if (i == (control ? controlRom_ : pcmRom_)) current = int(labels.size());
            labels.push_back(roms_[size_t(i)].description + "  (" + roms_[size_t(i)].fileName + ")");
            indexes.push_back(i);
        }
        const int chosen = control ? controlRom_ : pcmRom_;
        add(control ? "Control ROM" : "PCM ROM", chosen >= 0 ? roms_[size_t(chosen)].description : std::string("(none)"),
            [this, labels, indexes, current, control] {
                menus_.openChoice(control ? "Control ROM" : "PCM ROM", labels, current, [this, indexes, control](int index) { chooseRom(control == 1, indexes[size_t(index)]); });
            },
            nullptr, "D-110 ROMs play as the D-110; MT-32 or CM-32L ROMs as those units.", !labels.empty());
    }
    if (roms_.empty()) {
        info("No ROM images in this folder: put the D-110's CONTROLromcombo.bin and PCMromcombo.bin in a \"roms\" folder.", true);
    } else if (!synthError_.empty()) {
        info(synthError_, true);
    } else if (open) {
        info(std::string("Running: ") + kAnalogModeNames[int(status_.analogMode) + 1] + " analog output, " + std::to_string(status_.outputSampleRate) + " Hz");
    }
    stepper("Analog output", std::vector<std::string>(std::begin(kAnalogModeNames), std::end(kAnalogModeNames)), int(analogMode_) + 1,
            [this](int index) {
                analogMode_ = AnalogMode(index - 1);
                restartSynth();
                saveSettings();
            },
            "Emulation of the analog output stage. Auto picks the most accurate for the sample rate. Restarts the synth.");
    stepper("Resampler", std::vector<std::string>(std::begin(kQualityNames), std::end(kQualityNames)), resamplerQuality_, [this](int index) {
        resamplerQuality_ = index;
        restartSynth();
        saveSettings();
    }, "The quality of the conversion from the synth's rate to the output's. Restarts the synth.");

    heading("Sound");
    add("Output gain", percentText(engineOptions_.outputGain), nullptr, [this](int step) {
        engineOptions_.outputGain = std::clamp(std::round(engineOptions_.outputGain * 20.0f + float(step)) / 20.0f, 0.0f, 4.0f);
        setOptions();
    }, "The volume of the whole output (+ and - on the main screen too).");
    add("Reverb gain", percentText(engineOptions_.reverbGain), nullptr, [this](int step) {
        engineOptions_.reverbGain = std::clamp(std::round(engineOptions_.reverbGain * 20.0f + float(step)) / 20.0f, 0.0f, 4.0f);
        setOptions();
    }, "The reverb's volume against the dry sound.");
    const int defaultTune = status_.d110 && !status_.mt32Translation ? 0x40 : 0x4A;
    add("Master tune", open ? masterTuneText(status_.masterTune) : std::string("-"), [this, defaultTune] { engine_->setMasterTune(defaultTune); },
        [this](int step) { engine_->setMasterTune(std::clamp(int(status_.masterTune) + step, 0, 127)); },
        "A4 from 427.5 to 452.7 Hz, kept in the unit's memory (SysEx can set it too). Enter: the default, " + masterTuneText(defaultTune) + ".",
        open);
    {
        const int modes = status_.d110 ? 9 : 4;
        const int mode = std::min<int>(status_.reverbMode, modes - 1);
        std::vector<std::string> names;
        for (int i = 0; i < modes; i++) names.push_back(status_.d110 ? kD110ReverbNames[i] : kReverbModeNames[i]);
        stepper("Reverb type", names, mode, [this](int index) { engine_->setReverb(index, status_.reverbTime, status_.reverbLevel); },
                "The unit's reverb, in its memory like the patches set it.", open);
        add("Reverb time", open ? std::to_string(status_.reverbTime + 1) : std::string("-"), nullptr, [this](int step) {
            engine_->setReverb(status_.reverbMode, std::clamp(int(status_.reverbTime) + step, 0, 7), status_.reverbLevel);
        }, "", open);
        add("Reverb level", open ? std::to_string(status_.reverbLevel) : std::string("-"), nullptr, [this](int step) {
            engine_->setReverb(status_.reverbMode, status_.reverbTime, std::clamp(int(status_.reverbLevel) + step, 0, 7));
        }, "", open);
    }
    engineToggle("Reverb", &EngineOptions::reverbEnabled, "Off: no reverb at all, whatever the unit's setting.");
    engineToggle("Lock reverb", &EngineOptions::reverbOverridden, "Ignores reverb settings sent over MIDI.");
    if (status_.d110) {
        add("Reverb model", engineOptions_.dSeriesReverb ? "D-series" : "MT-32 chip", [this] {
            engineOptions_.dSeriesReverb = !engineOptions_.dSeriesReverb;
            setOptions();
        }, [this](int) {
            engineOptions_.dSeriesReverb = !engineOptions_.dSeriesReverb;
            setOptions();
        }, "D-series: all eight types of the D-110, D-10 and D-20 (tuned in the standalone's Reverb tuning window). MT-32 chip: "
           "mt32emu's model of the MT-32 family's reverb.");
    }
    engineToggle("Swap L/R", &EngineOptions::reversedStereo, "Left and right the other way round.");
    engineToggle("Nice panning", &EngineOptions::nicePanning, "All 15 panpot steps instead of the LA32's 8; with the D-110 ROMs, fine pan "
                                                              "from -64 to +64 and MIDI pan at full resolution.");

    heading("System");
    add("MT-32 translation", onOff(mt32Mode_), [this] { setMt32Mode(!mt32Mode_); }, [this](int) { setMt32Mode(!mt32Mode_); },
        "Plays music for the MT-32 (or CM-32L) as a translation box would: parts 1-8 on MIDI channels 2-9, rhythm on 10; off brings the "
        "D-110 setup back (T on the main screen too).");
    if (mt32PresetRom_ < 0) {
        info("MT-32 presets: D-110 stand-ins (an MT-32 or CM-32L control ROM in the ROM folder plays its own)");
    } else {
        stepper("MT-32 presets", {"D-110 stand-ins", "Chosen one by one", "The MT-32's own"}, int(mt32PresetMode_), [this](int index) {
            mt32PresetMode_ = Mt32Translator::PresetMode(index);
            engine_->setMt32PresetMode(mt32PresetMode_);
            cacheTime_ = -1.0;
            saveSettings();
        }, "What the MT-32's presets play while translating (the choices one by one are made in the standalone's MT-32 presets window).");
    }
    add("Roomy toms", onOff(mt32RoomyToms_), [this] {
        mt32RoomyToms_ = !mt32RoomyToms_;
        engine_->setMt32RoomyToms(mt32RoomyToms_);
        saveSettings();
    }, nullptr, "MT-32 toms on the D-110's TomTom1 set (roomier) instead of TomTom2, the MT-32's sample.");
    if (status_.d110) {
        add("Performance mode", onOff(performanceMode_), [this] { setPerformanceMode(!performanceMode_); },
            [this](int) { setPerformanceMode(!performanceMode_); },
            "The D-20's performance mode: parts 1 and 2 play the performance patch (upper and lower) on its channel; off brings the "
            "multi-timbral setup back.");
        add("Performance channel", std::to_string(performanceChannel_ + 1), nullptr, [this](int step) {
            performanceChannel_ = std::clamp(performanceChannel_ + step, 0, 15);
            if (performanceMode_) engine_->setPerformanceMode(true, performanceChannel_);
            saveSettings();
        }, "The MIDI channel performance mode plays on.");
        add("Control channel", engineOptions_.controlChannel < 16 ? std::to_string(engineOptions_.controlChannel + 1) : std::string("Off"),
            nullptr, [this](int step) {
                engineOptions_.controlChannel = uint8_t(std::clamp(int(engineOptions_.controlChannel) + step, 0, 16));
                setOptions();
            }, "Program changes on this channel select patches (I-11 to I-88) instead of timbres.");
        add("Unit number", std::to_string(engineOptions_.unitNumber), nullptr, [this](int step) {
            engineOptions_.unitNumber = uint8_t(std::clamp(int(engineOptions_.unitNumber) + step, 17, 32));
            setOptions();
        }, "The SysEx device number (17 = device ID 10H); messages for other units are ignored.");
        add("Part channels", "", [this] {
            menus_.openChoice("Part channels", {"D-110: parts 1-8, rhythm 10", "MT-32: parts 2-9, rhythm 10"}, -1,
                       [this](int index) { setChannels(index == 0 ? kD110Channels : kMT32Channels, true); });
        }, nullptr, "Sets every part's MIDI channel as the D-110 or the MT-32 has them.");
    }
    {
        std::vector<std::string> labels;
        int current = 0;
        for (int i = 0; i < int(std::size(kPartialCounts)); i++) {
            labels.push_back(std::to_string(kPartialCounts[i]) + (kPartialCounts[i] == kDefaultPartials ? " (original)" : ""));
            if (kPartialCounts[i] == partialCount_) current = i;
        }
        stepper("Partials", labels, current, [this](int index) {
            partialCount_ = kPartialCounts[index];
            restartSynth();
            saveSettings();
        }, "Polyphony: the D-110 has 32 partials, and a tone uses 1 to 4 per note. Restarts the synth (the memory is kept).");
    }
    add("16 parts", onOff(sixteenParts_), [this] {
        sixteenParts_ = !sixteenParts_;
        restartSynth();
        saveSettings();
    }, nullptr, "15 melodic parts and rhythm, one per MIDI channel (parts 9-15 on channels 9 and 11-16). Restarts the synth.",
        status_.d110 || sixteenParts_);

    heading("Memory");
    add("Load SysEx file", "", [this] {
        std::error_code ec;
        openFiles(!lastMidiFolder_.empty() && std::filesystem::is_directory(lastMidiFolder_, ec) ? lastMidiFolder_ : std::filesystem::current_path(ec));
    }, nullptr, "Patches, timbres, tones or a whole memory, from a .syx file (F on the main screen).");
    add("Save memory as SysEx", "", [this] {
        std::error_code ec;
        const std::filesystem::path folder = !lastMidiFolder_.empty() ? lastMidiFolder_ : std::filesystem::current_path(ec);
        menus_.openTextInput("Save memory", "A SysEx file for the memory, as a bulk dump a real D-110 accepts:", Platform::toUtf8(folder / "D-110 memory.syx"),
                      [this](const std::string& path) {
                          std::string error;
                          if (engine_->saveSysexFile(Platform::fromUtf8(path), DumpMemory, error)) {
                              addLog("Saved the memory to " + path);
                          } else {
                              addLog("SysEx: " + error);
                          }
                      });
    }, nullptr, "Tones, timbres, patches, rhythm setup and system settings.", open);
    add("Reset MIDI", "", [this] {
        engine_->resetMidiChannels();
        addLog("Reset MIDI: sound off, controllers reset, levels 100, pans centred");
    }, nullptr, "Stops all sound and resets every part's controllers, pedal and pitch bend, its level to 100 and its pan to "
                "the centre (X on the main screen).", open);
    add("Restart synth", "", [this] { resetSynth(); }, nullptr,
        "Power-cycles the emulated synth: with the D-110 ROMs its memory and parts are kept, every part's level at 100 "
        "and its pan centred.");
    add("Initialize memory", "", [this] { openInitializeConfirm(); }, nullptr,
        "The unit's memory back to its power-on state.", open);
}

void TuiApp::openMidiInputs() {
    refreshMidiPorts();
    menus_.open("MIDI inputs", [this](std::vector<MenuItem>& items) {
        auto info = [&](const std::string& text, bool problem) {
            for (const std::string& line : wrapText(text, kInfoWidth)) {
                MenuItem item;
                item.label = line;
                item.info = true;
                item.problem = problem;
                items.push_back(item);
            }
        };
        const std::string systemError = midi_->systemError();
        if (!systemError.empty()) info(systemError, true);
        if (!midiError_.empty()) info(midiError_, true);
        for (const std::string& name : midiPorts_) {
            const bool enabled = midi_->isOpen(name);
            MenuItem item;
            item.label = (enabled ? "[x] " : "[ ] ") + name;
            item.activate = [this, name, enabled] { setMidiPortEnabled(name, !enabled); };
            items.push_back(item);
        }
        for (const std::string& name : enabledMidiPorts_) {
            if (std::find(midiPorts_.begin(), midiPorts_.end(), name) != midiPorts_.end()) continue;
            MenuItem item;
            item.label = "[x] " + name;
            item.value = "not connected";
            item.activate = [this, name] { setMidiPortEnabled(name, false); };
            items.push_back(item);
        }
        if (midiPorts_.empty() && systemError.empty()) info("No MIDI inputs found.", false);
        const std::string own = midi_->ownPortName();
        if (!own.empty()) info("Programs can send to " + own + " themselves (" + midi_->ownPortHint() + ").", false);
        MenuItem refresh;
        refresh.label = "Look again";
        refresh.activate = [this] { refreshMidiPorts(); };
        items.push_back(refresh);
    });
}

void TuiApp::openRomPlay() {
    menus_.open("ROM Play", [this](std::vector<MenuItem>& items) {
        if (romSongs_.empty()) {
            MenuItem item;
            item.label = status_.open ? "This control ROM has no ROM Play songs." : "The synth is not running.";
            item.info = true;
            items.push_back(item);
            return;
        }
        for (int song = 0; song <= int(romSongs_.size()); song++) {
            MenuItem item;
            item.label = song < int(romSongs_.size()) ? std::to_string(song + 1) + "  " + romSongs_[size_t(song)].name : std::string("All songs (chain)");
            if (song == romPlaying_) item.value = "playing";
            item.activate = [this, song] {
                menus_.closeAll();
                startRomPlay(song);
            };
            items.push_back(item);
        }
        if (romPlaying_ >= 0) {
            MenuItem stop;
            stop.label = "Stop";
            stop.activate = [this] {
                menus_.closeAll();
                restoreAfterRomPlay();
            };
            items.push_back(stop);
        }
    });
    menus_.select(romPlaying_ >= 0 ? romPlaying_ : romSongChoice_);
}

void TuiApp::openFiles(const std::filesystem::path& folder) {
    menus_.openFiles(folder, isMidiOrSysexFile, "No MIDI (.mid) or SysEx (.syx) files here.", [this](const std::filesystem::path& path) {
        if (lowerText(Platform::toUtf8(path.extension())) == ".syx") addLog("SysEx file: " + Platform::toUtf8(path.filename()));
        openFile(path);
    });
}

void TuiApp::openInitializeConfirm() {
    menus_.open("Initialize memory?", [this](std::vector<MenuItem>& items) {
        for (const std::string& line : wrapText("Tones, timbres, patches, rhythm setup and system go back to their power-on state.", kInfoWidth)) {
            MenuItem text;
            text.label = line;
            text.info = true;
            items.push_back(text);
        }
        MenuItem cancel;
        cancel.label = "Cancel";
        cancel.activate = [this] { menus_.pop(); };
        items.push_back(cancel);
        MenuItem initialize;
        initialize.label = "Initialize";
        initialize.activate = [this] {
            menus_.closeAll();
            initializeMemory();
        };
        items.push_back(initialize);
    });
}

// ---------------------------------------------------------------------------------------------
// Settings

void TuiApp::chooseAudioDevice(const std::string& name) {
    audioDevice_ = name;
    startAudioAndSynth();
    audioDeviceList_ = audioDevices_;
    saveSettings();
}

void TuiApp::chooseRom(bool control, int index) {
    (control ? controlRom_ : pcmRom_) = index;
    restartSynth();
    saveSettings();
}

void TuiApp::setRomFolder(const std::string& folder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(Platform::fromUtf8(folder), ec)) addLog("ROMs: there is no folder " + folder);
    romFolder_ = Platform::fromUtf8(folder);
    scanRoms();
    restartSynth();
    saveSettings();
}

std::string TuiApp::midiInputsText() const {
    const std::vector<std::string> open = midi_->openPorts();
    std::string text;
    for (const std::string& name : open) text += (text.empty() ? "" : ", ") + name;
    const size_t missing = size_t(std::count_if(enabledMidiPorts_.begin(), enabledMidiPorts_.end(), [&](const std::string& name) {
        return std::find(open.begin(), open.end(), name) == open.end();
    }));
    if (missing > 0) text += (text.empty() ? "" : ", ") + std::to_string(missing) + " not connected";
    return text.empty() ? std::string("None") : text;
}
