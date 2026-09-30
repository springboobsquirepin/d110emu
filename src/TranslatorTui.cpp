#include "TranslatorTui.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "Platform.h"
#include "PresetNames.h"

using namespace Tui::Ansi;
using Tui::Key;
using Tui::Menus;
using Tui::Screen;
using Tui::Style;
using Tui::ansiStyle;
using Tui::findByName;
using Tui::lowerText;
using Tui::wrapText;

namespace {

constexpr int kMinColumns = 60;
constexpr int kMinRows = 16;
constexpr double kLightSeconds = 0.15;  // An activity light stays on this long
constexpr int kLabelWidth = 12;         // The status screen's labels

const char* const kPartLabels[TranslatorCore::kPartCount] = {"1", "2", "3", "4", "5", "6", "7", "8", "R"};
const char* const kPresetModes[] = {"The unit's closest presets", "Closest, MT-32's own if none fits", "The MT-32's own presets"};
const int kShifts[] = {-24, -12, 0, 12, 24};
const char* const kShiftNames[] = {"-2 oct", "-1 oct", "0", "+1 oct", "+2 oct"};

std::string onOff(bool on) {
    return on ? "On" : "Off";
}

// Tone cache slot 0-63 as the unit shows it: i11-i88.
std::string slotName(int slot) {
    return std::string("i") + char('1' + slot / 8) + char('1' + slot % 8);
}

std::string channelText(uint8_t channel) {
    return channel < 16 ? std::to_string(channel + 1) : std::string("Off");
}

std::string shiftText(int shift) {
    for (int i = 0; i < 5; i++) {
        if (kShifts[i] == shift) return kShiftNames[i];
    }
    return (shift > 0 ? "+" : "") + std::to_string(shift);
}

bool isSysexFile(const std::filesystem::path& path) {
    const std::string extension = lowerText(Platform::toUtf8(path.extension()));
    return extension == ".syx" || extension == ".dat";
}

bool isTranslatable(const std::filesystem::path& path) {
    return isSysexFile(path) || TranslatorCore::isMidiFile(path);
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// The D-series tones a preset may play: a11-b88, then r01-r64.
std::vector<int> standInTones() {
    std::vector<int> tones;
    for (int tone = 0; tone < 256; tone++) {
        if (tone < 128 || tone >= 192) tones.push_back(tone);
    }
    return tones;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Setup

double TranslatorTui::now() const {
    return clock_ >= 0.0 ? clock_ : TranslatorCore::now();
}

void TranslatorTui::loadFrontEndSettings() {
    // The command line's choices, as if made in the menus: the ports open with them, and they are kept.
    const CommandLine& line = commandLine_;
    const Mt32Translator::Target target = line.target == "d110" ? Mt32Translator::Target::D110 : Mt32Translator::Target::D20;
    if (!line.target.empty() && target != pipeSettings_.target) setTarget(target);
    if (line.unitNumber >= 17 && line.unitNumber <= 32) pipeSettings_.unitNumber = line.unitNumber;
    if (!options_.enableMidi) return;
    const std::vector<std::string> inputs = inputs_->listPorts();
    for (const std::string& wanted : line.inputs) {
        const std::string found = findByName(inputs, wanted);
        if (found.empty()) {
            addLog("MIDI: no input named " + wanted + " yet; it opens when it appears");
            pendingInputs_.push_back(wanted);
        } else if (!contains(enabledInputs_, found)) {
            enabledInputs_.push_back(found);
        }
    }
    // A port named that is not there yet is opened when it appears (the one set before is not opened meanwhile).
    auto choose = [&](const std::string& wanted, const std::vector<std::string>& ports, std::string& name, std::string& pending,
                      const char* what) {
        if (wanted.empty()) return;
        if (lowerText(wanted) == "none") {
            name.clear();
            return;
        }
        const std::string found = findByName(ports, wanted);
        if (found.empty()) {
            addLog(std::string("MIDI: no ") + what + " named " + wanted + " yet; it opens when it appears");
            pending = wanted;
            name.clear();
        } else {
            name = found;
        }
    };
    const std::vector<std::string> outputs = MidiOutputPort::listPorts();
    choose(line.unitOutput, outputs, unitPortName_, pendingUnitOutput_, "output");
    choose(line.replyOutput, outputs, replyPortName_, pendingReplyOutput_, "output");
    choose(line.unitInput, inputs, unitInputName_, pendingUnitInput_, "input");
}

void TranslatorTui::onLogAdded(const std::string& line) {
    if (tuiOptions_.headless) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    } else if (echo_) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

bool TranslatorTui::takeRedrawRequest() {
    const bool redraw = redraw_;
    redraw_ = false;
    return redraw;
}

std::string TranslatorTui::summary() const {
    std::string text = "Unit: ";
    text += pipeSettings_.target == Mt32Translator::Target::D20 ? "D-10/D-20" : "D-110";
    text += ", unit number " + std::to_string(pipeSettings_.unitNumber);
    text += "\nFrom the program: " + inputsText();
    if (!ownInputName().empty()) text += "\nPrograms can send to " + ownInputName();
    text += "\nTo the unit: " + outputText(unitPort_, unitPortName_);
    text += "\nReplies: " + outputText(replyPort_, replyPortName_);
    text += "\nFrom the unit: " + unitInputText();
    text += "\nPresets: " + presetModeText();
    if (!options_.settingsFile.empty()) text += "\nSettings: " + Platform::toUtf8(options_.settingsFile);
    return text;
}

void TranslatorTui::openFile(const std::filesystem::path& path) {
    if (isMidiFile(path)) {
        addLog("A MIDI file is not sent: translate it with T (or mt32translate) to play it on the unit: " + Platform::toUtf8(path.filename()));
        return;
    }
    sendSysexFile(path);
}

void TranslatorTui::settingsChanged() {
    applyPipeSettings();
    saveSettings();
}

// ---------------------------------------------------------------------------------------------
// Frames

void TranslatorTui::tick() {
    update();
    if (!options_.enableMidi || !inputs_) return;
    const double time = now();

    // Ports named on the command line that were not there at the start open once they appear (the core's check of the
    // ports every two seconds brings the lists up to date).
    for (auto wanted = pendingInputs_.begin(); wanted != pendingInputs_.end();) {
        const std::string found = findByName(inputPorts_, *wanted);
        if (found.empty()) {
            ++wanted;
            continue;
        }
        openInput(found, true);
        if (inputs_->isOpen(found)) addLog("MIDI input " + found + ": connected");
        saveSettings();
        wanted = pendingInputs_.erase(wanted);
    }
    auto pendingOutput = [&](std::string& pending, const MidiOutputPort& port, void (TranslatorTui::*open)(const std::string&)) {
        if (pending.empty()) return;
        const std::string found = findByName(outputPorts_, pending);
        if (found.empty()) return;
        pending.clear();
        (this->*open)(found);  // Kept, and tried again every two seconds while it does not open
        if (port.isOpen()) addLog("MIDI output " + found + ": connected");
        saveSettings();
    };
    pendingOutput(pendingUnitOutput_, unitPort_, &TranslatorTui::openUnitOutput);
    pendingOutput(pendingReplyOutput_, replyPort_, &TranslatorTui::openReplyOutput);
    if (!pendingUnitInput_.empty()) {
        const std::string found = findByName(inputPorts_, pendingUnitInput_);
        if (!found.empty()) {
            pendingUnitInput_.clear();
            openUnitInput(found);
            if (unitInputs_->isOpen(found)) addLog("MIDI input " + found + " (from the unit): connected");
            saveSettings();
        }
    }

    // The activity lights.
    uint64_t fromInputs = 0;
    for (const std::string& name : inputs_->openPorts()) {
        const uint32_t count = inputs_->messageCount(name);
        fromInputs += count;
        if (count != inputCounts_[name]) {
            inputCounts_[name] = count;
            inputActivity_[name] = time;
        }
    }
    const MidiPipe::Stats stats = pipe_.stats();
    const uint64_t own = stats.received > fromInputs ? stats.received - fromInputs : 0;
    if (own != ownCount_) {
        ownCount_ = own;
        ownActivity_ = time;
    }
    if (stats.sent != sentCount_) {
        sentCount_ = stats.sent;
        sentActivity_ = time;
    }
    if (stats.replies != replyCount_) {
        replyCount_ = stats.replies;
        replyActivity_ = time;
    }
    if (!unitInputName_.empty()) {
        const uint32_t count = unitInputs_->messageCount(unitInputName_);
        if (count != unitInputCount_) {
            unitInputCount_ = count;
            unitInputActivity_ = time;
        }
    }
}

void TranslatorTui::draw(Screen& screen) {
    screen.clear();
    if (screen.columns() < kMinColumns || screen.rows() < kMinRows) {
        const std::string text = "MT32Translator needs a terminal of at least " + std::to_string(kMinColumns) + " x " + std::to_string(kMinRows);
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

void TranslatorTui::drawTitle(Screen& screen, const std::string& title) {
    const int width = screen.columns();
    const Style bar = ansiStyle(-1, Tui::Reverse);
    screen.fill(0, 0, width, 1, U' ', bar);
    const int x = 1 + screen.text(1, 0, "MT32Translator", ansiStyle(-1, Tui::Reverse | Tui::Bold)) + 2;
    std::string right = pipeSettings_.target == Mt32Translator::Target::D20 ? "D-10/D-20" : "D-110";
    right += "  unit " + std::to_string(pipeSettings_.unitNumber);
    const int rightWidth = Tui::textWidth(right);
    screen.text(x, 0, title, bar, std::max(0, width - x - rightWidth - 3));
    if (rightWidth + x + 2 < width) screen.text(width - rightWidth - 1, 0, right, bar);
}

void TranslatorTui::drawStatus(Screen& screen) {
    const int height = screen.rows();
    drawTitle(screen, "MT-32 MIDI in, D-series MIDI out");
    // The gaps between the sections go first on short terminals.
    const bool gaps = height >= 22;
    int y = gaps ? 2 : 1;
    y = drawPorts(screen, y) + (gaps ? 1 : 0);
    y = drawSetup(screen, y) + (gaps ? 1 : 0);
    drawCounters(screen, y++);
    drawLogLines(screen, y, height - 1 - y);
    Tui::drawHints(screen, {{"Q", "Quit"}, {"?", "Help"}, {"O", "Options"}, {"P", "Inputs"}, {"R", "Reset"}, {"S", "Send"}, {"T", "Translate"},
                            {"Tab", "Log"}});
}

int TranslatorTui::drawPorts(Screen& screen, int y) {
    const int width = screen.columns();
    const Style label = ansiStyle(-1, Tui::Dim);
    const Style missing = ansiStyle(kYellow);
    const double time = now();
    auto light = [&](int x, int row, double when) {
        const bool lit = when >= 0.0 && time - when < kLightSeconds;
        return x + screen.text(x, row, "●", lit ? ansiStyle(kBrightGreen, Tui::Bold) : ansiStyle(kGrey)) + 1;
    };
    auto put = [&](int x, int row, const std::string& text, const Style& style) {
        if (x >= width) return x;
        return x + screen.text(x, row, text, style, width - x);
    };

    // From the program: the inputs chosen, with their lights; those not there; where programs can send themselves.
    put(0, y, "MIDI in", label);
    int x = kLabelWidth;
    const std::string systemError = inputs_ && options_.enableMidi ? inputs_->systemError() : std::string();
    if (!systemError.empty()) {
        put(x, y, systemError, ansiStyle(kRed, Tui::Bold));
    } else {
        bool any = false;
        for (const std::string& name : enabledInputs_) {
            const bool open = inputs_ && inputs_->isOpen(name);
            if (open) {
                const auto activity = inputActivity_.find(name);
                x = light(x, y, activity != inputActivity_.end() ? activity->second : -1.0);
                x = put(x, y, name, Style()) + 2;
            } else {
                x = put(x, y, name + " (not connected)", missing) + 2;
            }
            any = true;
        }
        for (const std::string& wanted : pendingInputs_) {
            x = put(x, y, wanted + " (not connected)", missing) + 2;
            any = true;
        }
        const std::string own = ownInputName();
        if (!any && own.empty()) x = put(x, y, "none chosen (P)", label) + 2;
        if (!own.empty()) {
            x = light(x, y, ownActivity_);
            put(x, y, "programs: " + own, label);
        }
    }

    // To the unit, the replies, from the unit.
    auto output = [&](int row, const char* name, const MidiOutputPort& port, const std::string& wanted, const std::string& pending,
                      double activity) {
        put(0, row, name, label);
        int column = kLabelWidth;
        if (wanted.empty() && pending.empty()) {
            put(column, row, "none", label);
        } else if (!pending.empty()) {
            put(column, row, pending + " (not connected)", missing);
        } else if (!port.connected()) {
            put(column, row, wanted + " (not connected)", missing);
        } else {
            column = light(column, row, activity);
            put(column, row, wanted, Style());
        }
    };
    output(y + 1, "To unit", unitPort_, unitPortName_, pendingUnitOutput_, sentActivity_);
    output(y + 2, "Replies", replyPort_, replyPortName_, pendingReplyOutput_, replyActivity_);
    put(0, y + 3, "From unit", label);
    if (unitInputName_.empty() && pendingUnitInput_.empty()) {
        put(kLabelWidth, y + 3, "none", label);
    } else if (!pendingUnitInput_.empty() || !unitInputs_->isOpen(unitInputName_)) {
        put(kLabelWidth, y + 3, (pendingUnitInput_.empty() ? unitInputName_ : pendingUnitInput_) + " (not connected)", missing);
    } else {
        put(light(kLabelWidth, y + 3, unitInputActivity_), y + 3, unitInputName_, Style());
    }
    return y + 4;
}

int TranslatorTui::drawSetup(Screen& screen, int y) {
    const int width = screen.columns();
    const Style label = ansiStyle(-1, Tui::Dim);
    auto line = [&](const char* name, const std::string& text, const Style& style) {
        screen.text(0, y, name, label);
        screen.text(kLabelWidth, y, text, style, width - kLabelWidth);
        y++;
    };
    const bool d20 = pipeSettings_.target == Mt32Translator::Target::D20;
    char device[16];
    std::snprintf(device, sizeof(device), "%02XH", pipeSettings_.unitNumber - 1);
    line("Unit", std::string(d20 ? "D-10/D-20" : "D-110") + ", unit number " + std::to_string(pipeSettings_.unitNumber) + " (device " + device + "), " +
                     (pipeSettings_.memoryInUnit ? "memories written to the unit" : "memories kept here") +
                     (pipeSettings_.roomyToms ? ", roomy toms" : ""),
         Style());
    if (d20) {
        std::string channels;
        for (int part = 0; part < kPartCount; part++) {
            channels += std::string(part > 0 ? "  " : "") + kPartLabels[part] + ":" + channelText(pipeSettings_.unitChannels[part]);
        }
        line("Channels", channels, Style());
    }
    std::string presets = presetModeText();
    if (presets_) {
        presets += "  (" + presets_->description + ")";
    } else if (!presetError_.empty()) {
        presets += "  (" + presetError_ + ")";
    }
    line("Presets", presets, Style());
    if (pipeSettings_.memoryInUnit) {
        line("Tone cache", "Off (while the memories are written to the unit)", label);
    } else if (!cacheEnabled_) {
        line("Tone cache", "Off", label);
    } else {
        const Mt32Translator::CacheStats stats = pipe_.toneCacheStats();
        const std::string slots = slotName(cacheFirst_) + "-" + slotName(cacheLast_) + ": ";
        if (stats.failed) {
            line("Tone cache", slots + "the unit refused a write: off (memory protect?)", ansiStyle(kRed, Tui::Bold));
        } else {
            line("Tone cache", slots + std::to_string(stats.used) + " of " + std::to_string(stats.slots) + " hold tones, " +
                                   std::to_string(stats.hits) + " found, " + std::to_string(stats.misses) + " stored",
                 Style());
        }
    }
    std::string timing = "SysEx pause " + std::to_string(pipeSettings_.sysexGapMs) + " ms";
    if (pipeSettings_.reduceLoad) timing += ", reduced MIDI load";
    if (powerOnAtStart_) timing += ", MT-32 power-on setup at start";
    line("Timing", timing, Style());
    return y;
}

void TranslatorTui::drawCounters(Screen& screen, int y) {
    const MidiPipe::Stats stats = pipe_.stats();
    const Style label = ansiStyle(-1, Tui::Dim);
    const int width = screen.columns();
    int x = 0;
    auto add = [&](const std::string& name, const std::string& value, const Style& style) {
        if (x >= width) return;
        x += screen.text(x, y, name + " ", label, width - x);
        if (x >= width) return;
        x += screen.text(x, y, value, style, width - x) + 3;
    };
    add("Received", std::to_string(stats.received), Style());
    add("Sent", std::to_string(stats.sent) + " (" + std::to_string(stats.sysexBytesSent) + " SysEx bytes)", Style());
    add("Waiting", std::to_string(stats.queued), Style());
    add("Replies", std::to_string(stats.replies), Style());
    add("Transfers", std::to_string(stats.handshakes), Style());
    if (stats.dropped > 0) add("Not passed on", std::to_string(stats.dropped), ansiStyle(kRed, Tui::Bold));
}

void TranslatorTui::drawLogLines(Screen& screen, int y, int rows) {
    // The newest log lines, then what keeps the translation from working, if anything.
    const int width = screen.columns();
    std::string problem;
    if (options_.enableMidi) {
        if (!MidiOutputPort::systemError().empty()) {
            problem = "MIDI output: " + MidiOutputPort::systemError();
        } else if (!portError_.empty()) {
            problem = portError_;
        } else if (unitPortName_.empty() && pendingUnitOutput_.empty()) {
            problem = "No output to the unit: press O and choose one.";
        }
    }
    const int logRows = std::min(int(log_.size()), rows - (problem.empty() ? 0 : 1));
    const Style dim = ansiStyle(-1, Tui::Dim);
    for (int row = 0; row < logRows; row++) screen.text(0, y + row, log_[log_.size() - size_t(logRows - row)], dim, width);
    if (!problem.empty() && rows > 0) screen.text(0, y + std::max(logRows, 0), problem, ansiStyle(kRed, Tui::Bold), width);
}

void TranslatorTui::drawLogView(Screen& screen) {
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
    for (int row = 0; row < rows && first + row < count; row++) screen.text(0, 1 + row, lines[size_t(first + row)], Style(), screen.columns());
    if (log_.empty()) screen.text(0, 1, "Nothing yet.", ansiStyle(-1, Tui::Dim));
    Tui::drawHints(screen, {{"Tab", "The status"}, {"↑↓ PgUp PgDn", "Scroll"}, {"End", "Newest"}, {"Q", "Quit"}});
}

void TranslatorTui::drawHelp(Screen& screen) {
    static const char* const kKeys[][2] = {
        {"O", "Options: the MIDI ports, the unit, the presets, the tone cache, the timing"},
        {"P", "The MIDI inputs the MT-32 program plays into"},
        {"R", "MT-32 reset: the MT-32's power-on setup, to the unit"},
        {"N", "All notes off on the unit's channels"},
        {"S", "Send a SysEx file (.syx, or a game's .dat) through the translation"},
        {"T", "Translate a file: a SysEx file into one for the unit's memory, a MIDI file into one that plays on it"},
        {"Tab", "The log"},
        {"Q", "Quit (the settings and the tone cache are kept for the next start)"},
    };
    const int count = int(sizeof(kKeys) / sizeof(kKeys[0]));
    const int width = std::min(screen.columns() - 4, 78);
    const int x = (screen.columns() - width) / 2;
    std::vector<std::pair<std::string, std::string>> lines;
    for (int i = 0; i < count; i++) {
        const std::vector<std::string> wrapped = wrapText(kKeys[i][1], width - 12);
        for (size_t line = 0; line < wrapped.size(); line++) lines.emplace_back(line == 0 ? kKeys[i][0] : "", wrapped[line]);
    }
    const int height = std::min(screen.rows() - 2, int(lines.size()) + 3);
    const int y = std::max(1, (screen.rows() - height) / 2);
    screen.fill(x, y, width, height, U' ', Style());
    screen.box(x, y, width, height, Style(), "Keys");
    for (int i = 0; i < int(lines.size()) && i < height - 3; i++) {
        screen.text(x + 2, y + 2 + i, lines[size_t(i)].first, ansiStyle(-1, Tui::Bold), 6);
        screen.text(x + 9, y + 2 + i, lines[size_t(i)].second, Style(), width - 11);
    }
    screen.text(x + width - 20, y + height - 1, " Any key closes ", ansiStyle(-1, Tui::Dim));
}

// ---------------------------------------------------------------------------------------------
// Keys

void TranslatorTui::onKey(const Key& key) {
    if (key.code == Key::Char && key.ctrl && key.ch == U'c') {
        requestQuit();
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

void TranslatorTui::onStatusKey(const Key& key) {
    if (key.is(U'q') || key.code == Key::F10) {
        requestQuit();
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
    case Key::F1: showHelp_ = true; return;
    case Key::F2: openOptions(); return;
    default: break;
    }
    if (key.code != Key::Char || key.ctrl) return;
    switch (key.ch) {
    case U'?': case U'h': case U'H': showHelp_ = true; return;
    case U'o': case U'O': openOptions(); return;
    case U'p': case U'P': openInputs(); return;
    case U'r': case U'R': pipe_.powerOn(); return;
    case U'n': case U'N':
        pipe_.allNotesOff();
        addLog("All notes off and controllers reset on all channels of the unit");
        return;
    case U's': case U'S': openSendFile(); return;
    case U't': case U'T': openTranslateFile(); return;
    default: return;
    }
}

// ---------------------------------------------------------------------------------------------
// Menus

void TranslatorTui::openOptions() {
    menus_.open("Options", [this](std::vector<MenuItem>& items) { buildOptions(items); });
}

void TranslatorTui::buildOptions(std::vector<MenuItem>& items) {
    auto add = [&](const std::string& label, const std::string& value, std::function<void()> activate, std::function<void(int)> adjust,
                   const std::string& help, bool enabled = true) { Menus::item(items, label, value, std::move(activate), std::move(adjust), help, enabled); };
    // An on/off setting: Enter and the arrows turn it over.
    auto toggle = [&](const std::string& label, bool& value, const std::string& help, bool enabled = true) {
        bool* field = &value;
        add(label, onOff(value), [this, field] {
            *field = !*field;
            settingsChanged();
        }, [this, field](int) {
            *field = !*field;
            settingsChanged();
        }, help, enabled);
    };
    // A step through a list of values, and the list itself on Enter.
    auto stepper = [&](const std::string& label, const std::vector<std::string>& labels, int current, std::function<void(int)> choose,
                       const std::string& help, bool enabled = true) {
        auto chosen = std::make_shared<std::function<void(int)>>(std::move(choose));
        const int count = int(labels.size());
        add(label, current >= 0 && current < count ? labels[size_t(current)] : std::string("-"),
            [this, label, labels, current, chosen] { menus_.openChoice(label, labels, current, *chosen); },
            [current, count, chosen](int step) {
                const int next = std::clamp(current + step, 0, count - 1);
                if (next != current) (*chosen)(next);
            },
            help, enabled);
    };

    Menus::heading(items, "MIDI ports");
    add("From the program", inputsText(), [this] { openInputs(); }, nullptr,
        "The MIDI inputs the game, sequencer or emulator plays into, as it would play into an MT-32; remembered by name, and "
        "opened again when they come back.");
    add("To the unit", outputText(unitPort_, unitPortName_), [this] { openOutputChoice(true); }, nullptr,
        "The MIDI output the D-110, D-10 or D-20 is connected to.");
    add("Replies", outputText(replyPort_, replyPortName_), [this] { openOutputChoice(false); }, nullptr,
        "The output back to the MT-32 program, for games that load their sounds with a handshake transfer and wait for each "
        "packet's answer (many X68000 and PC-98 games).");
    add("From the unit", unitInputText(), [this] { openUnitInputChoice(); }, nullptr,
        "Optional: the input from the unit's MIDI OUT. When the unit refuses a tone cache write, the cache goes off and the "
        "parts get their tones directly.");

    Menus::heading(items, "Unit");
    const bool d20 = pipeSettings_.target == Mt32Translator::Target::D20;
    stepper("Unit", {"D-110", "D-10 / D-20"}, d20 ? 1 : 0, [this](int index) {
        setTarget(index == 1 ? Mt32Translator::Target::D20 : Mt32Translator::Target::D110);
        settingsChanged();
    }, "D-110: the MT-32's map, part channels and memories included. D-10/D-20 in multi-timbral mode: its parts' channels are "
       "set on its panel (Part channels, below).");
    char device[16];
    std::snprintf(device, sizeof(device), "%02XH", pipeSettings_.unitNumber - 1);
    add("Unit number", std::to_string(pipeSettings_.unitNumber) + " (device " + device + ")", nullptr, [this](int step) {
        pipeSettings_.unitNumber = std::clamp(pipeSettings_.unitNumber + step, 17, 32);
        settingsChanged();
    }, "The unit number set on the unit (17-32). The translator sends its SysEx there, whatever device ID the program uses.");
    if (d20) {
        std::string channels;
        for (int part = 0; part < kPartCount; part++) channels += std::string(part > 0 ? " " : "") + channelText(pipeSettings_.unitChannels[part]);
        add("Part channels", channels, [this] { openChannels(); }, nullptr,
            "The MIDI channels of the unit's parts 1-8 and rhythm, as set in its MIDI function menu. Each part of the MT-32 program "
            "plays on its part here.");
    }
    toggle("Write memories to the unit", pipeSettings_.memoryInUnit,
           "On: the program's patches and timbres go into the unit's timbre memory and tones i11-i88. Off: kept here and sent "
           "as parts select them (a D-20 needs this).");
    toggle("Master volume as CC 7", pipeSettings_.masterVolumeAsVolume,
           "The D-series units have no master volume: with this on, the MT-32's master volume scales the parts' MIDI volume.");
    stepper("Toms", {"MT-32's (TomTom2)", "Roomy (TomTom1)"}, pipeSettings_.roomyToms ? 1 : 0, [this](int index) {
        pipeSettings_.roomyToms = index == 1;
        settingsChanged();
    }, "The MT-32's toms are the unit's TomTom2 set; Roomy plays the TomTom1 set. Send an MT-32 reset to hear the change.");

    Menus::heading(items, "MT-32 presets");
    const int mode = int(pipeSettings_.presetMode);
    stepper("Presets", {kPresetModes[0], kPresetModes[1], kPresetModes[2]}, mode, [this](int index) {
        if (index > 0 && !presets_) {
            addLog("The MT-32's own presets need an MT-32 or CM-32L control ROM (Control ROM, below)");
            return;
        }
        pipeSettings_.presetMode = Mt32Translator::PresetMode(index);
        settingsChanged();
    }, "The unit's closest presets, octave-corrected; the MT-32's own timbre (from its control ROM) where none fits; or the "
       "MT-32's own for all.");
    if (presets_) {
        Menus::info(items, presets_->description + " (" + Platform::toUtf8(presetRom_.filename()) + ")");
    } else if (!presetError_.empty()) {
        Menus::info(items, presetError_, true);
    } else {
        Menus::info(items, "No MT-32 control ROM: put one in a \"roms\" folder, or choose it below.");
    }
    add("Control ROM", presetRom_.empty() ? std::string("None") : Platform::toUtf8(presetRom_.filename()), [this] { openControlRom(); }, nullptr,
        "An MT-32 or CM-32L control ROM image, the source of the MT-32's presets.");
    add("Choose presets", "", [this] { openPresetList(); }, nullptr,
        "What each MT-32 preset plays on the unit: its closest preset (with a key shift), or the MT-32's own.");
    add("All built in", "", [this] {
        setPresetChoicesBuiltIn();
        settingsChanged();
        addLog("MT-32 presets: the choices that come with the translator");
    }, nullptr, "Back to the choices that come with the translator.");

    Menus::heading(items, "Tone cache");
    toggle("Keep tones in the unit", cacheEnabled_,
           "Stores the tones sent in the unit's internal tones below, so that selecting one again is a short message. Overwrites "
           "them; needs Memory Protect off.",
           !pipeSettings_.memoryInUnit);
    add("First tone", slotName(cacheFirst_), nullptr, [this](int step) {
        cacheFirst_ = std::clamp(cacheFirst_ + step, 0, cacheLast_);
        settingsChanged();
    }, "", cacheEnabled_ && !pipeSettings_.memoryInUnit);
    add("Last tone", slotName(cacheLast_), nullptr, [this](int step) {
        cacheLast_ = std::clamp(cacheLast_ + step, cacheFirst_, 63);
        settingsChanged();
    }, "", cacheEnabled_ && !pipeSettings_.memoryInUnit);
    add("Preload", "", [this] { pipe_.preloadToneCache(); }, nullptr,
        "Stores the program's timbres now, so that its songs start without delay: best at a quiet moment, after it has loaded its "
        "sounds.",
        cacheEnabled_ && !pipeSettings_.memoryInUnit);
    add("Forget", "", [this] { pipe_.clearToneCache(); }, nullptr,
        "Forgets what the tones hold (nothing is sent); they are stored again as they are used.", cacheEnabled_ && !pipeSettings_.memoryInUnit);

    Menus::heading(items, "Timing");
    add("SysEx pause", std::to_string(pipeSettings_.sysexGapMs) + " ms", nullptr, [this](int step) {
        pipeSettings_.sysexGapMs = std::clamp(pipeSettings_.sysexGapMs + step, 0, 100);
        settingsChanged();
    }, "After each SysEx message: its transmission (0.32 ms a byte), then this pause, so that the unit can store the data. "
       "Raise it if the unit misses parts of transfers.");
    toggle("Reduce MIDI load", pipeSettings_.reduceLoad,
           "Leaves out controller values the unit already has, and merges those that wait. Older D-series firmware delays "
           "envelopes when busy.");
    toggle("Power-on setup at start", powerOnAtStart_,
           "Sends the MT-32's power-on state when the translator starts, so that the unit matches an MT-32 just switched on.");

    Menus::heading(items, "Actions");
    add("MT-32 reset", "", [this] {
        menus_.closeAll();
        pipe_.powerOn();
    }, nullptr, "Sends the MT-32's power-on setup, as an MT-32 reset does (R on the main screen).");
    add("All notes off", "", [this] {
        menus_.closeAll();
        pipe_.allNotesOff();
        addLog("All notes off and controllers reset on all channels of the unit");
    }, nullptr, "All Notes Off and Reset All Controllers on all 16 channels (N).");
    add("Send a SysEx file", "", [this] { openSendFile(); }, nullptr,
        "Translates a SysEx file for the MT-32 (.syx, or a game's .dat) and sends it to the unit, as if the program had sent it (S).");
    add("Translate a file", "", [this] { openTranslateFile(); }, nullptr,
        "Saves a translated copy: a SysEx file for the unit's memory, or a MIDI file that plays on the unit after the MT-32's "
        "power-on setup (T).");
}

void TranslatorTui::openInputs() {
    refreshPorts();
    menus_.open("MIDI inputs from the program", [this](std::vector<MenuItem>& items) {
        const std::string systemError = inputs_->systemError();
        if (!systemError.empty()) Menus::info(items, systemError, true);
        if (!portError_.empty()) Menus::info(items, portError_, true);
        for (const std::string& name : inputPorts_) {
            if (name == unitInputName_) continue;  // The unit's answers
            const bool enabled = inputs_->isOpen(name);
            MenuItem item;
            item.label = (enabled ? "[x] " : "[ ] ") + name;
            item.activate = [this, name, enabled] {
                portError_.clear();
                openInput(name, !enabled);
                saveSettings();
            };
            items.push_back(item);
        }
        for (const std::string& name : enabledInputs_) {
            if (contains(inputPorts_, name)) continue;
            MenuItem item;
            item.label = "[x] " + name;
            item.value = "not connected";
            item.activate = [this, name] {
                openInput(name, false);
                saveSettings();
            };
            items.push_back(item);
        }
        if (inputPorts_.empty() && systemError.empty()) Menus::info(items, "No MIDI inputs found.");
        if (!ownInputName().empty()) Menus::info(items, "Programs on this computer can send to " + ownInputName() + " themselves (" + ownInputHint() + ").");
        MenuItem refresh;
        refresh.label = "Look again";
        refresh.activate = [this] { refreshPorts(); };
        items.push_back(refresh);
    });
}

void TranslatorTui::openOutputChoice(bool unit) {
    refreshPorts();
    std::vector<std::string> labels = {"None"};
    labels.insert(labels.end(), outputPorts_.begin(), outputPorts_.end());
    const std::string& current = unit ? unitPortName_ : replyPortName_;
    int chosen = 0;
    for (size_t i = 0; i < outputPorts_.size(); i++) {
        if (outputPorts_[i] == current) chosen = int(i) + 1;
    }
    menus_.openChoice(unit ? "To the unit" : "Replies to the program", labels, chosen, [this, labels, unit](int index) {
        const std::string name = index == 0 ? std::string() : labels[size_t(index)];
        portError_.clear();
        (unit ? pendingUnitOutput_ : pendingReplyOutput_).clear();
        unit ? openUnitOutput(name) : openReplyOutput(name);
        saveSettings();
    });
}

void TranslatorTui::openUnitInputChoice() {
    refreshPorts();
    std::vector<std::string> labels = {"None"};
    labels.insert(labels.end(), inputPorts_.begin(), inputPorts_.end());
    int chosen = 0;
    for (size_t i = 0; i < inputPorts_.size(); i++) {
        if (inputPorts_[i] == unitInputName_) chosen = int(i) + 1;
    }
    menus_.openChoice("From the unit", labels, chosen, [this, labels](int index) {
        portError_.clear();
        pendingUnitInput_.clear();
        openUnitInput(index == 0 ? std::string() : labels[size_t(index)]);
        saveSettings();
    });
}

void TranslatorTui::openChannels() {
    menus_.open("Part channels on the unit", [this](std::vector<MenuItem>& items) {
        std::vector<std::string> labels;
        for (int channel = 0; channel <= 16; channel++) labels.push_back(channelText(uint8_t(channel)));
        for (int part = 0; part < kPartCount; part++) {
            const int current = pipeSettings_.unitChannels[part];
            Menus::item(items, part < 8 ? "Part " + std::to_string(part + 1) : std::string("Rhythm"), channelText(uint8_t(current)),
                        [this, part, labels, current] {
                            menus_.openChoice(part < 8 ? "Part " + std::to_string(part + 1) : std::string("Rhythm"), labels, current, [this, part](int index) {
                                pipeSettings_.unitChannels[part] = uint8_t(index);
                                settingsChanged();
                            });
                        },
                        [this, part](int step) {
                            pipeSettings_.unitChannels[part] = uint8_t(std::clamp(int(pipeSettings_.unitChannels[part]) + step, 0, 16));
                            settingsChanged();
                        },
                        "As set in the unit's MIDI function menu (Off: the part takes no MIDI).");
        }
        Menus::item(items, "1-8, R 10", "", [this] {
            for (int part = 0; part < 8; part++) pipeSettings_.unitChannels[part] = uint8_t(part);
            pipeSettings_.unitChannels[8] = 9;
            settingsChanged();
        }, nullptr, "Parts 1-8 on channels 1-8, rhythm on 10.");
        Menus::item(items, "2-9, R 10", "", [this] {
            for (int part = 0; part < 8; part++) pipeSettings_.unitChannels[part] = uint8_t(part + 1);
            pipeSettings_.unitChannels[8] = 9;
            settingsChanged();
        }, nullptr, "Parts 1-8 on channels 2-9, rhythm on 10, as the MT-32 has them.");
    });
}

void TranslatorTui::openPresetList() {
    menus_.open("MT-32 presets", [this](std::vector<MenuItem>& items) {
        if (pipeSettings_.presetMode == Mt32Translator::PresetMode::Exact && presets_) {
            Menus::info(items, "All of them play the MT-32's own in this mode.");
        }
        for (int timbre = 0; timbre < 128; timbre++) {
            const bool changed = !(pipeSettings_.presetChoices[size_t(timbre)] == Mt32Translator::defaultPresetChoice(timbre));
            char number[8];
            std::snprintf(number, sizeof(number), "%c%-2d ", timbre < 64 ? 'A' : 'B', timbre % 64 + 1);
            Menus::item(items, number + presetName(timbre), presetPlays(timbre) + (changed ? "  *" : ""), [this, timbre] { openPreset(timbre); },
                        nullptr, "Enter: what it plays. *: not the choice that comes with the translator.");
        }
    });
}

void TranslatorTui::openPreset(int timbre) {
    char number[8];
    std::snprintf(number, sizeof(number), "%c%d ", timbre < 64 ? 'A' : 'B', timbre % 64 + 1);
    menus_.open(number + presetName(timbre), [this, timbre](std::vector<MenuItem>& items) {
        Mt32Translator::PresetChoice& choice = pipeSettings_.presetChoices[size_t(timbre)];
        const bool hybrid = pipeSettings_.presetMode == Mt32Translator::PresetMode::Hybrid && presets_;
        const bool exactAll = pipeSettings_.presetMode == Mt32Translator::PresetMode::Exact && presets_;
        const bool exact = exactAll || (hybrid && choice.exact);
        Menus::item(items, "Plays", exact ? "The MT-32's own" : "The unit's preset", [this, timbre] {
            Mt32Translator::PresetChoice& edited = pipeSettings_.presetChoices[size_t(timbre)];
            edited.exact = !edited.exact;
            settingsChanged();
        }, [this, timbre](int) {
            Mt32Translator::PresetChoice& edited = pipeSettings_.presetChoices[size_t(timbre)];
            edited.exact = !edited.exact;
            settingsChanged();
        }, "The unit's closest preset, or the MT-32's own timbre (in the mode \"Closest, MT-32's own if none fits\", with a control ROM).",
                    hybrid);
        Menus::item(items, "Unit's preset", dSeriesToneLabel(choice.tone), [this, timbre] {
            const std::vector<int> tones = standInTones();
            std::vector<std::string> labels;
            int current = 0;
            for (size_t i = 0; i < tones.size(); i++) {
                labels.push_back(dSeriesToneLabel(tones[i]));
                if (tones[i] == pipeSettings_.presetChoices[size_t(timbre)].tone) current = int(i);
            }
            menus_.openChoice("Unit's preset", labels, current, [this, timbre, tones](int index) {
                pipeSettings_.presetChoices[size_t(timbre)].tone = uint8_t(tones[size_t(index)]);
                settingsChanged();
            });
        }, nullptr, "The D-series preset (a11-b88) or rhythm tone (r01-r64) it plays.", !exact);
        int shift = 2;
        for (int i = 0; i < 5; i++) {
            if (kShifts[i] == choice.keyShift) shift = i;
        }
        Menus::item(items, "Key shift", kShiftNames[shift], nullptr, [this, timbre, shift](int step) {
            pipeSettings_.presetChoices[size_t(timbre)].keyShift = int8_t(kShifts[std::clamp(shift + step, 0, 4)]);
            settingsChanged();
        }, "For a unit's preset an octave or two away from the MT-32's.", !exact);
        const Mt32Translator::PresetChoice builtIn = Mt32Translator::defaultPresetChoice(timbre);
        Menus::info(items, "Built in: " + dSeriesToneLabel(builtIn.tone) + (builtIn.keyShift != 0 ? " " + shiftText(builtIn.keyShift) : std::string()) +
                               (builtIn.exact ? ", the MT-32's own in \"Closest, MT-32's own if none fits\"" : ""));
        Menus::item(items, "Built in", "", [this, timbre] {
            pipeSettings_.presetChoices[size_t(timbre)] = Mt32Translator::defaultPresetChoice(timbre);
            settingsChanged();
        }, nullptr, "Back to the choice that comes with the translator.", !(choice == builtIn));
    });
}

void TranslatorTui::openSendFile() {
    menus_.openFiles(startFolder(), isSysexFile, "No SysEx (.syx) or MT-32 data (.dat) files here.", [this](const std::filesystem::path& path) {
        lastFolder_ = path.parent_path();
        sendSysexFile(path);
    });
}

void TranslatorTui::openTranslateFile() {
    menus_.openFiles(startFolder(), isTranslatable, "No MIDI (.mid) or SysEx (.syx, .dat) files here.", [this](const std::filesystem::path& path) {
        lastFolder_ = path.parent_path();
        if (!canTranslate(path)) return;
        menus_.openTextInput("Translate " + Platform::toUtf8(path.filename()), "Where the translated copy goes:",
                             Platform::toUtf8(path.parent_path() / Platform::fromUtf8(translatedName(path))), [this, path](const std::string& target) {
                                 if (!target.empty()) translateFileTo(path, Platform::fromUtf8(target));
                             });
    });
}

void TranslatorTui::openControlRom() {
    std::error_code ec;
    const std::filesystem::path folder = !presetRom_.empty() && std::filesystem::is_directory(presetRom_.parent_path(), ec) ? presetRom_.parent_path()
                                                                                                                             : startFolder();
    menus_.openFiles(folder, [](const std::filesystem::path&) { return true; }, "No files here.", [this](const std::filesystem::path& path) {
        presetRom_ = path;
        loadPresets();
        settingsChanged();
        addLog(presets_ ? "MT-32 presets from " + presets_->description : "Not an MT-32 control ROM: " + Platform::toUtf8(path.filename()) +
                                                                              (presetError_.empty() ? std::string() : " (" + presetError_ + ")"));
    });
}

// ---------------------------------------------------------------------------------------------
// Texts

std::string TranslatorTui::presetName(int timbre) const {
    return presets_ ? Mt32Presets::name(presets_->melodic[size_t(timbre)]) : std::string(kMt32PresetNames[timbre]);
}

std::string TranslatorTui::presetPlays(int timbre) const {
    const Mt32Translator::PresetChoice& choice = pipeSettings_.presetChoices[size_t(timbre)];
    const bool hybrid = pipeSettings_.presetMode == Mt32Translator::PresetMode::Hybrid && presets_;
    const bool exactAll = pipeSettings_.presetMode == Mt32Translator::PresetMode::Exact && presets_;
    if (exactAll || (hybrid && choice.exact)) return "MT-32's own";
    return dSeriesToneLabel(choice.tone) + (choice.keyShift != 0 ? " " + shiftText(choice.keyShift) : std::string());
}

std::string TranslatorTui::presetModeText() const {
    return kPresetModes[std::clamp(int(pipeSettings_.presetMode), 0, 2)];
}

std::string TranslatorTui::inputsText() const {
    std::string text;
    size_t missing = 0;
    for (const std::string& name : enabledInputs_) {
        if (inputs_ && inputs_->isOpen(name)) {
            text += (text.empty() ? "" : ", ") + name;
        } else {
            missing++;
        }
    }
    missing += pendingInputs_.size();
    if (missing > 0) text += (text.empty() ? "" : ", ") + std::to_string(missing) + " not connected";
    return text.empty() ? std::string("None") : text;
}

std::string TranslatorTui::outputText(const MidiOutputPort& port, const std::string& wanted) const {
    const std::string& pending = &port == &unitPort_ ? pendingUnitOutput_ : pendingReplyOutput_;
    if (!pending.empty()) return pending + " (not connected)";
    if (wanted.empty()) return "None";
    return port.connected() ? wanted : wanted + " (not connected)";
}

std::string TranslatorTui::unitInputText() const {
    if (!pendingUnitInput_.empty()) return pendingUnitInput_ + " (not connected)";
    if (unitInputName_.empty()) return "None";
    return unitInputs_ && unitInputs_->isOpen(unitInputName_) ? unitInputName_ : unitInputName_ + " (not connected)";
}

std::filesystem::path TranslatorTui::startFolder() const {
    std::error_code ec;
    if (!lastFolder_.empty() && std::filesystem::is_directory(lastFolder_, ec)) return lastFolder_;
    return std::filesystem::current_path(ec);
}
