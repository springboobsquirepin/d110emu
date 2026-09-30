#include "ToneEditor.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Piano.h"
#include "UiStyle.h"
#include "imgui.h"

namespace {

constexpr size_t kMaxUndo = 200;
constexpr float kSustainUnits = 40.0f;  // Width of the sustain in the envelope graphs, in time units (0-100 per segment)

const ImU32 kPartialColors[Tone::kPartials] = {IM_COL32(240, 165, 60, 255), IM_COL32(80, 190, 240, 255), IM_COL32(125, 210, 110, 255),
                                               IM_COL32(225, 115, 220, 255)};
const ImU32 kSynthBox = IM_COL32(70, 110, 170, 255);
const ImU32 kPcmBox = IM_COL32(170, 110, 60, 255);

ImU32 withAlpha(ImU32 color, float alpha) {
    const unsigned a = unsigned(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (color & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
}

// ImGui takes slider texts as format strings.
std::string formatText(const std::string& text) {
    std::string out;
    for (char c : text) {
        out += c;
        if (c == '%') out += '%';
    }
    return out;
}

int nameCharFilter(ImGuiInputTextCallbackData* data) {
    return data->EventChar < 32 || data->EventChar > 126 ? 1 : 0;
}

// A structure as a small diagram: the pair's partials (S or P) mixed, ring modulated or panned apart.
void structureDiagram(int value, int first, float width, float height) {
    const Tone::Structure& s = Tone::structure(value);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float small = fs * 0.78f;
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 text = IM_COL32(240, 240, 240, 255);
    const float boxW = small * 1.9f, boxH = height * 0.42f;
    const ImVec2 topMin(origin.x, origin.y), bottomMin(origin.x, origin.y + height - boxH);
    auto box = [&](ImVec2 min, bool pcm, int partial) {
        dl->AddRectFilled(min, ImVec2(min.x + boxW, min.y + boxH), pcm ? kPcmBox : kSynthBox, 2.0f);
        char label[8];
        std::snprintf(label, sizeof(label), "%c%d", pcm ? 'P' : 'S', partial);
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0.0f, label);
        dl->AddText(ImGui::GetFont(), small, ImVec2(min.x + (boxW - size.x) * 0.5f, min.y + (boxH - size.y) * 0.5f), text, label);
    };
    box(topMin, s.firstPcm, first);
    box(bottomMin, s.secondPcm, first + 1);
    const ImVec2 topOut(topMin.x + boxW, topMin.y + boxH * 0.5f), bottomOut(bottomMin.x + boxW, bottomMin.y + boxH * 0.5f);
    const float midY = origin.y + height * 0.5f;
    const float right = origin.x + width;
    const float x1 = origin.x + boxW + (width - boxW) * 0.35f;
    const float x2 = origin.x + boxW + (width - boxW) * 0.7f;
    const float r = small * 0.42f;
    auto ring = [&](ImVec2 c) {
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(150, 60, 60, 255), 2.0f);
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(small * 0.9f, FLT_MAX, 0.0f, "R");
        dl->AddText(ImGui::GetFont(), small * 0.9f, ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), text, "R");
    };
    auto plus = [&](ImVec2 c) {
        dl->AddCircle(c, r * 0.8f, line, 12, 1.2f);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.5f, c.y), line, 1.2f);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), line, 1.2f);
    };
    switch (s.mix) {
    case Tone::Mix::Mix:
        dl->AddLine(topOut, ImVec2(x2 - r, midY), line, 1.2f);
        dl->AddLine(bottomOut, ImVec2(x2 - r, midY), line, 1.2f);
        plus(ImVec2(x2, midY));
        dl->AddLine(ImVec2(x2 + r, midY), ImVec2(right, midY), line, 1.2f);
        break;
    case Tone::Mix::RingPlusFirst:
        dl->AddLine(topOut, ImVec2(x1 - r, midY), line, 1.2f);
        dl->AddLine(bottomOut, ImVec2(x1 - r, midY), line, 1.2f);
        ring(ImVec2(x1, midY));
        dl->AddLine(ImVec2(x1 + r, midY), ImVec2(x2 - r, midY), line, 1.2f);
        dl->AddLine(topOut, ImVec2(x2, topOut.y), line, 1.2f);
        dl->AddLine(ImVec2(x2, topOut.y), ImVec2(x2, midY - r), line, 1.2f);
        plus(ImVec2(x2, midY));
        dl->AddLine(ImVec2(x2 + r, midY), ImVec2(right, midY), line, 1.2f);
        break;
    case Tone::Mix::Ring:
        dl->AddLine(topOut, ImVec2(x1 - r, midY), line, 1.2f);
        dl->AddLine(bottomOut, ImVec2(x1 - r, midY), line, 1.2f);
        ring(ImVec2(x1, midY));
        dl->AddLine(ImVec2(x1 + r, midY), ImVec2(right, midY), line, 1.2f);
        break;
    case Tone::Mix::Stereo: {
        dl->AddLine(topOut, ImVec2(x2, topOut.y), line, 1.2f);
        dl->AddLine(bottomOut, ImVec2(x2, bottomOut.y), line, 1.2f);
        dl->AddText(ImGui::GetFont(), small, ImVec2(x2 + 2.0f, topOut.y - small * 0.5f), line, "L");
        dl->AddText(ImGui::GetFont(), small, ImVec2(x2 + 2.0f, bottomOut.y - small * 0.5f), line, "R");
        break;
    }
    }
}

}  // namespace

ToneEditor::ToneEditor() : tone_(Tone::initialTone()), original_(tone_), unit_(tone_) {}

void ToneEditor::setModel(Tone::Model model) {
    model_ = model;
}

// ---------------------------------------------------------------------------------------------
// The tone and what the unit holds

Tone::Data ToneEditor::sentTone() const {
    Tone::Data tone = comparing_ ? original_ : tone_;
    if (solo_ >= 0 && !comparing_) tone[Tone::Common::PartialMute] = uint8_t(1 << solo_);
    return tone;
}

void ToneEditor::beginStep() {
    if (stepOpen_) return;
    undo_.push_back(tone_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    stepOpen_ = true;
}

void ToneEditor::noteChange() {
    lastChange_ = now_;
    restrikePending_ = true;
}

void ToneEditor::sync(Host& host) {
    const Tone::Data want = sentTone();
    // Runs of changed bytes; a gap shorter than a message's overhead (10 bytes) is sent along rather than split.
    int first = -1, last = -1;
    bool sent = false;
    for (int i = 0; i <= Tone::kSize; i++) {
        const bool differs = i < Tone::kSize && want[size_t(i)] != unit_[size_t(i)];
        if (differs && first >= 0 && i - last > 10) {
            host.writeTone(first, want.data() + first, last - first + 1);
            sent = true;
            first = -1;
        }
        if (differs) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first >= 0) {
        host.writeTone(first, want.data() + first, last - first + 1);
        sent = true;
    }
    unit_ = want;
    if (sent) noteChange();
}

void ToneEditor::loadTone(const Tone::Data& tone) {
    Tone::Data clamped = tone;
    Tone::clamp(clamped);
    if (clamped != tone_) {
        undo_.push_back(tone_);
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
        redo_.clear();
    }
    tone_ = clamped;
    original_ = clamped;
    unit_ = tone;
    solo_ = -1;
    comparing_ = false;
    stepOpen_ = false;
}

void ToneEditor::takeExternal(int offset, const uint8_t* data, int length) {
    if (offset < 0 || length <= 0 || offset + length > Tone::kSize) return;
    Tone::Data next = tone_;
    std::memcpy(next.data() + offset, data, size_t(length));
    Tone::clamp(next);
    std::memcpy(unit_.data() + offset, data, size_t(length));
    if (next == tone_) return;
    undo_.push_back(tone_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    tone_ = next;
    stepOpen_ = false;
}

void ToneEditor::replaceTone(Host& host, const Tone::Data& tone, bool asOriginal) {
    Tone::Data clamped = tone;
    Tone::clamp(clamped);
    if (clamped != tone_) {
        undo_.push_back(tone_);
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
        redo_.clear();
    }
    stepOpen_ = false;
    tone_ = clamped;
    if (asOriginal) original_ = clamped;
    solo_ = -1;
    comparing_ = false;
    sendAll(host);
}

void ToneEditor::sendAll(Host& host) {
    unit_ = sentTone();
    host.writeTone(0, unit_.data(), Tone::kSize);
    noteChange();
}

void ToneEditor::setByte(Host& host, int offset, int value) {
    stepOpen_ = false;
    set(host, offset, value);
    stepOpen_ = false;
}

void ToneEditor::set(Host& host, int offset, int value) {
    if (offset < 0 || offset >= Tone::kSize) return;
    const int low = offset < Tone::kNameLength ? 32 : 0;
    const uint8_t clamped = uint8_t(std::clamp(value, low, int(Tone::maxValue(offset))));
    if (tone_[size_t(offset)] == clamped) return;
    beginStep();
    tone_[size_t(offset)] = clamped;
    sync(host);
}

void ToneEditor::settle(Host& host) {
    solo_ = -1;
    comparing_ = false;
    sync(host);
}

void ToneEditor::solo(Host& host, int partial) {
    solo_ = partial >= 0 && partial < Tone::kPartials ? partial : -1;
    sync(host);
}

void ToneEditor::compare(Host& host, bool on) {
    comparing_ = on;
    sync(host);
}

bool ToneEditor::undo(Host& host) {
    if (undo_.empty() || comparing_) return false;
    redo_.push_back(tone_);
    tone_ = undo_.back();
    undo_.pop_back();
    stepOpen_ = false;
    sync(host);
    return true;
}

bool ToneEditor::redo(Host& host) {
    if (redo_.empty() || comparing_) return false;
    undo_.push_back(tone_);
    tone_ = redo_.back();
    redo_.pop_back();
    stepOpen_ = false;
    sync(host);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Audition

void ToneEditor::playNote(Host& host, int key, int velocity) {
    key = std::clamp(key, 0, 127);
    if (held_[size_t(key)] > 0) host.noteOff(key);
    host.noteOn(key, std::clamp(velocity, 1, 127));
    held_[size_t(key)] = std::clamp(velocity, 1, 127);
}

void ToneEditor::stopNote(Host& host, int key) {
    if (key < 0 || key > 127 || held_[size_t(key)] == 0) return;
    // Another of the editor's keys may hold the same note (the keyboard, Hold, Play).
    if ((key == mouseNote_) + (key == holdKey_) + (key == timedNote_) > 1) return;
    host.noteOff(key);
    held_[size_t(key)] = 0;
}

void ToneEditor::releaseNotes(Host& host) {
    for (int key = 0; key < 128; key++) {
        if (held_[size_t(key)] > 0) host.noteOff(key);
        held_[size_t(key)] = 0;
    }
    mouseNote_ = -1;
    timedNote_ = -1;
    holdKey_ = -1;
    audition_.hold = false;
}

void ToneEditor::update(Host& host, double now) {
    now_ = now;
    // Held notes play again after a change: at once after a pause, then at most every 0.35 s while a control moves,
    // and once more when it has settled. First, so that notes started below already have the change.
    if (!audition_.restrike) restrikePending_ = false;
    if (restrikePending_ && (now - lastChange_ >= 0.12 || now - lastRestrike_ >= 0.35)) {
        restrikePending_ = false;
        lastRestrike_ = now;
        for (int key = 0; key < 128; key++) {
            if (held_[size_t(key)] == 0) continue;
            host.noteOff(key);
            host.noteOn(key, held_[size_t(key)]);
        }
        host.restrike();
    }
    if (timedNote_ >= 0 && now >= timedOff_) {
        const int key = timedNote_;
        timedNote_ = -1;
        if (key != mouseNote_ && key != holdKey_) stopNote(host, key);
    }
    if (audition_.repeat && now >= nextRepeat_) {
        if (timedNote_ >= 0) stopNote(host, timedNote_);
        timedNote_ = audition_.key;
        playNote(host, timedNote_, audition_.velocity);
        timedOff_ = now + audition_.lengthMs / 1000.0;
        nextRepeat_ = now + std::max(audition_.repeatMs, audition_.lengthMs + 50) / 1000.0;
    }
    const int wantHold = audition_.hold ? audition_.key : -1;
    if (wantHold != holdKey_) {
        const int old = holdKey_;
        holdKey_ = wantHold;
        if (old >= 0 && old != mouseNote_ && old != timedNote_ && held_[size_t(old)] > 0) {
            host.noteOff(old);
            held_[size_t(old)] = 0;
        }
        if (wantHold >= 0) playNote(host, wantHold, audition_.velocity);
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing

void ToneEditor::draw(Host& host, double now) {
    update(host, now);
    ImGui::PushID(this);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undo(host);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal)) redo(host);
    drawHeader(host);
    const float fs = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float auditionHeight = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y + (pianoShown_ ? fs * 3.2f + style.ItemSpacing.y : 0.0f);
    ImGui::BeginDisabled(comparing_);
    drawEnvelopes(host);
    drawGrid(host, std::max(ImGui::GetContentRegionAvail().y - auditionHeight, fs * 8.0f));
    ImGui::EndDisabled();
    drawAudition(host, now);
    ImGui::PopID();
    if (!ImGui::IsAnyItemActive()) stepOpen_ = false;
}

void ToneEditor::drawHeader(Host& host) {
    const float fs = ImGui::GetFontSize();
    ImGui::BeginDisabled(comparing_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    char name[Tone::kNameLength + 1] = {};
    const std::string current = Tone::name(tone_);
    std::snprintf(name, sizeof(name), "%s", current.c_str());
    ImGui::SetNextItemWidth(fs * 7.0f);
    if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_CallbackCharFilter, nameCharFilter)) {
        beginStep();
        Tone::setName(tone_, name);
        sync(host);
    }
    if (ImGui::IsItemActivated()) beginStep();

    ImGui::SameLine(0.0f, fs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Envelope");
    ImGui::SameLine();
    int envMode = tone_[Tone::Common::EnvMode];
    if (ImGui::RadioButton("Normal", envMode == 0)) set(host, Tone::Common::EnvMode, 0);
    ImGui::SameLine();
    if (ImGui::RadioButton("No sustain", envMode == 1)) set(host, Tone::Common::EnvMode, 1);
    UiStyle::setItemTooltip("ENV mode. No sustain: notes play their envelopes through to the end and ignore note off, "
                          "for drums and other decaying sounds.");
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, fs);
    ImGui::BeginDisabled(!canUndo() || comparing_);
    if (ImGui::Button("Undo")) undo(host);
    ImGui::EndDisabled();
    UiStyle::setItemTooltip(UI_CTRL "+Z");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo() || comparing_);
    if (ImGui::Button("Redo")) redo(host);
    ImGui::EndDisabled();
    UiStyle::setItemTooltip(UI_CTRL "+Y");
    ImGui::SameLine();
    bool comparing = comparing_;
    if (comparing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Compare")) compare(host, !comparing_);
    if (comparing) ImGui::PopStyleColor();
    UiStyle::setItemTooltip("Plays the tone as it was loaded, until you click again. Editing is paused meanwhile.");
    ImGui::SameLine();
    ImGui::BeginDisabled(comparing_);
    if (ImGui::Button("Init")) ImGui::OpenPopup("init");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("init")) {
        if (ImGui::MenuItem("Initialise the tone")) replaceTone(host, Tone::initialTone(), false);
        UiStyle::setItemTooltip("A sawtooth on partial 1 through an open filter (can be undone)");
        ImGui::EndPopup();
    }
    if (comparing_) {
        ImGui::SameLine(0.0f, fs);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Comparing: the original plays");
    } else if (modified()) {
        ImGui::SameLine(0.0f, fs);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Edited");
        UiStyle::setItemTooltip("The tone differs from the one loaded. Write it to memory or save it to keep it.");
    }

    ImGui::BeginDisabled(comparing_);
    drawStructure(host, 0);
    ImGui::SameLine(0.0f, fs * 1.5f);
    drawStructure(host, 1);
    ImGui::EndDisabled();
}

void ToneEditor::drawStructure(Host& host, int which) {
    const float fs = ImGui::GetFontSize();
    const int offset = which == 0 ? Tone::Common::Structure12 : Tone::Common::Structure34;
    const int first = which == 0 ? 1 : 3;
    const int value = tone_[size_t(offset)];
    ImGui::PushID(which);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Structure %d-%d", first, first + 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 11.5f);
    const std::string preview = std::to_string(value + 1) + ": " + Tone::structureText(value, first);
    if (ImGui::BeginCombo("##structure", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
        for (int s = 0; s < 13; s++) {
            ImGui::PushID(s);
            const std::string label = std::to_string(s + 1) + ": " + Tone::structureText(s, first);
            const ImVec2 pos = ImGui::GetCursorPos();
            if (ImGui::Selectable("##pick", s == value, 0, ImVec2(fs * 15.0f, fs * 1.9f))) set(host, offset, s);
            ImGui::SetCursorPos(pos);
            structureDiagram(s, first, fs * 4.2f, fs * 1.9f);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    UiStyle::setItemTooltip("How partials %d and %d are combined: S = synthesizer, P = PCM sample, R = ring modulator.", first, first + 1);
    ImGui::SameLine();
    structureDiagram(value, first, fs * 4.2f, ImGui::GetFrameHeight());
    ImGui::EndGroup();
    ImGui::PopID();
}

void ToneEditor::drawEnvelopes(Host& host) {
    const float fs = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = std::floor((ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f);
    const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.2f, fs * 4.0f, fs * 5.6f);  // Smaller in a short window
    drawEnvelope(host, Tone::Envelope::Pitch, width, height);
    ImGui::SameLine();
    drawEnvelope(host, Tone::Envelope::Tvf, width, height);
    ImGui::SameLine();
    drawEnvelope(host, Tone::Envelope::Tva, width, height);
}

void ToneEditor::drawEnvelope(Host& host, Tone::Envelope envelope, float width, float height) {
    const float fs = ImGui::GetFontSize();
    ImGui::PushID(int(envelope));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("graph", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    const char* title = envelope == Tone::Envelope::Pitch ? "Pitch envelope" : envelope == Tone::Envelope::Tvf ? "TVF envelope" : "TVA envelope";
    dl->AddText(ImVec2(origin.x + 6.0f, origin.y + 3.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), title);

    const std::vector<Tone::EnvelopePoint> points = Tone::envelopePoints(model_, envelope);
    int attackSegments = 0;
    for (const Tone::EnvelopePoint& point : points) {
        if (point.timeOffset >= 0 && !point.afterKeyOff) attackSegments++;
    }
    const float totalUnits = float(attackSegments) * 100.0f + kSustainUnits + 100.0f;
    const float left = origin.x + 6.0f, right = origin.x + width - 6.0f;
    const float top = origin.y + fs * 1.3f, bottom = origin.y + height - 5.0f;
    const float xScale = (right - left) / totalUnits;
    const float yScale = (bottom - top) / 100.0f;
    auto toScreen = [&](float units, float level) { return ImVec2(left + units * xScale, bottom - level * yScale); };
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border);
    if (envelope == Tone::Envelope::Pitch) dl->AddLine(toScreen(0.0f, 50.0f), toScreen(totalUnits, 50.0f), grid);

    struct Handle {
        int partial;
        int point;
        ImVec2 pos;
    };
    std::vector<Handle> handles;
    for (int order = 0; order < Tone::kPartials; order++) {
        const int partial = (focus_ + 1 + order) % Tone::kPartials;  // The focused partial last, on top
        if (envelope == Tone::Envelope::Tvf && Tone::isPcm(tone_, partial)) continue;  // No filter on PCM partials
        const uint8_t* p = tone_.data() + Tone::partialBase(partial);
        std::vector<ImVec2> line;
        float x = 0.0f, lastLevel = 0.0f;
        for (size_t i = 0; i < points.size(); i++) {
            const Tone::EnvelopePoint& point = points[i];
            if (point.afterKeyOff) {
                x += kSustainUnits;
                line.push_back(toScreen(x, lastLevel));  // Key off
            }
            if (point.timeOffset >= 0) x += float(p[point.timeOffset]);
            const float level = point.levelOffset >= 0 ? float(p[point.levelOffset]) : float(point.fixedLevel);
            line.push_back(toScreen(x, level));
            handles.push_back({partial, int(i), line.back()});
            lastLevel = level;
        }
        const bool on = Tone::partialOn(tone_, partial);
        const bool focused = partial == focus_;
        const ImU32 color = withAlpha(kPartialColors[partial], on ? (focused ? 1.0f : 0.75f) : 0.3f);
        dl->AddPolyline(line.data(), int(line.size()), color, 0, focused ? 2.2f : 1.2f);
        if (focused || hovered) {
            for (const Handle& handle : handles) {
                if (handle.partial == partial) dl->AddCircleFilled(handle.pos, focused ? 3.5f : 2.5f, color);
            }
        }
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    auto nearest = [&]() {
        float best = fs * fs * 0.45f;
        int found = -1;
        for (int i = int(handles.size()) - 1; i >= 0; i--) {  // Top-most first
            const float dx = handles[size_t(i)].pos.x - mouse.x, dy = handles[size_t(i)].pos.y - mouse.y;
            if (dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                found = i;
            }
        }
        return found;
    };
    if (activated) {
        drag_.envelope = -1;
        const int found = nearest();
        if (found >= 0) {
            const Handle& handle = handles[size_t(found)];
            const Tone::EnvelopePoint& point = points[size_t(handle.point)];
            const uint8_t* p = tone_.data() + Tone::partialBase(handle.partial);
            drag_ = {int(envelope), handle.partial, handle.point, point.timeOffset >= 0 ? p[point.timeOffset] : 0,
                     point.levelOffset >= 0 ? p[point.levelOffset] : 0, mouse.x, mouse.y};
            focus_ = handle.partial;
            beginStep();
        }
    }
    if (active && drag_.envelope == int(envelope)) {
        const Tone::EnvelopePoint& point = points[size_t(drag_.point)];
        const int base = Tone::partialBase(drag_.partial);
        if (point.timeOffset >= 0) set(host, base + point.timeOffset, int(std::lround(float(drag_.startTime) + (mouse.x - drag_.mouseX) / xScale)));
        if (point.levelOffset >= 0) {
            set(host, base + point.levelOffset, int(std::lround(float(drag_.startLevel) - (mouse.y - drag_.mouseY) / yScale)));
        }
    } else if (!active && drag_.envelope == int(envelope)) {
        drag_.envelope = -1;
    }
    if (hovered && !active) {
        const int found = nearest();
        if (found >= 0) {
            const Handle& handle = handles[size_t(found)];
            const Tone::EnvelopePoint& point = points[size_t(handle.point)];
            const uint8_t* p = tone_.data() + Tone::partialBase(handle.partial);
            std::string text = "Partial " + std::to_string(handle.partial + 1);
            for (int offset : {point.timeOffset, point.levelOffset}) {
                const Tone::Param* param = offset >= 0 ? Tone::findParam(offset) : nullptr;
                if (param != nullptr) text += "\n" + Tone::paramLabel(model_, *param) + ": " + Tone::formatValue(model_, *param, p[offset]);
            }
            if (point.timeOffset >= 0 || point.levelOffset >= 0) text += "\nDrag to change";
            UiStyle::setTooltip("%s", text.c_str());
        } else {
            UiStyle::setTooltip("Drag a point: sideways for its time, up and down for its level.\nThe partials' colours are "
                              "those of the columns below; switched-off partials are faint.");
        }
    }
    ImGui::PopID();
}

void ToneEditor::drawGrid(Host& host, float height) {
    const float fs = ImGui::GetFontSize();
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchSame;
    if (!ImGui::BeginTable("partials", 1 + Tone::kPartials, flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, fs * 9.0f);
    for (int partial = 0; partial < Tone::kPartials; partial++) {
        ImGui::TableSetupColumn(("Partial " + std::to_string(partial + 1)).c_str(), ImGuiTableColumnFlags_WidthStretch);
    }
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Partial");
    for (int partial = 0; partial < Tone::kPartials; partial++) {
        ImGui::TableSetColumnIndex(partial + 1);
        drawPartialHeader(host, partial);
    }

    bool open = true;
    int group = -1;
    for (const Tone::Param& param : Tone::partialParams()) {
        if (int(param.group) != group) {
            group = int(param.group);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            open = ImGui::TreeNodeEx(Tone::groupName(param.group), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns |
                                                                       ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        }
        if (!open) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        const std::string label = Tone::paramLabel(model_, param);
        ImGui::TextUnformatted(label.c_str());
        if (ImGui::IsItemHovered()) {
            std::string tip = std::string(param.display) + "   (offset " + std::to_string(param.offset) + ", 0-" + std::to_string(param.max) + ")";
            if (!param.pcm) tip += "\nSynthesizer partials only";
            if (!param.synth) tip += "\nPCM partials only";
            if (model_ == Tone::Model::D110 && param.offset == Tone::Partial::PenvSustain) {
                tip += "\nThe D-110's MIDI implementation calls this \"always 0\"; its parameter table lists -50 to +50.";
            }
            if (Tone::hiddenOnPanel(model_, param.offset)) tip += "\nThe D-10/D-20's panel does not show this parameter, but the unit plays it.";
            tip += "\nRight-click a value: set all partials, or the initial value. " UI_CTRL "+wheel steps it (Shift: by 10).";
            UiStyle::setTooltip("%s", tip.c_str());
        }
        for (int partial = 0; partial < Tone::kPartials; partial++) {
            ImGui::TableSetColumnIndex(partial + 1);
            drawCell(host, partial, param);
        }
    }
    ImGui::EndTable();
}

void ToneEditor::drawPartialHeader(Host& host, int partial) {
    ImGui::PushID(partial);
    bool on = Tone::partialOn(tone_, partial);
    if (ImGui::Checkbox("##on", &on)) {
        set(host, Tone::Common::PartialMute, tone_[Tone::Common::PartialMute] ^ (1 << partial));
        focus_ = partial;
    }
    UiStyle::setItemTooltip("Partial %d on or off (partial mute)", partial + 1);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPartialColors[partial]), "%d %s", partial + 1,
                       Tone::isPcm(tone_, partial) ? "PCM" : "Synth");
    if (ImGui::IsItemClicked()) focus_ = partial;
    ImGui::SameLine();
    const bool soloed = solo_ == partial;
    if (soloed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.55f, 0.15f, 1.0f));
    if (ImGui::SmallButton("S")) solo(host, soloed ? -1 : partial);
    if (soloed) ImGui::PopStyleColor();
    UiStyle::setItemTooltip("Solo: only this partial sounds, until you click again. Writing or saving ends it.");
    ImGui::SameLine();
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("partial");
    if (ImGui::BeginPopup("partial")) {
        const int base = Tone::partialBase(partial);
        if (ImGui::MenuItem("Copy")) {
            std::memcpy(clipboard_.data(), tone_.data() + base, Tone::kPartialSize);
            clipboardFull_ = true;
        }
        if (ImGui::MenuItem("Paste", nullptr, false, clipboardFull_)) {
            beginStep();
            std::memcpy(tone_.data() + base, clipboard_.data(), Tone::kPartialSize);
            Tone::clamp(tone_);
            sync(host);
        }
        if (ImGui::BeginMenu("Swap with")) {
            for (int other = 0; other < Tone::kPartials; other++) {
                if (other == partial) continue;
                if (ImGui::MenuItem(("Partial " + std::to_string(other + 1)).c_str())) {
                    beginStep();
                    std::swap_ranges(tone_.begin() + base, tone_.begin() + base + Tone::kPartialSize, tone_.begin() + Tone::partialBase(other));
                    sync(host);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Initialise")) {
            beginStep();
            Tone::initPartial(tone_, partial);
            sync(host);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void ToneEditor::drawCell(Host& host, int partial, const Tone::Param& param) {
    const int base = Tone::partialBase(partial);
    const int offset = base + param.offset;
    const int value = tone_[size_t(offset)];
    const bool pcm = Tone::isPcm(tone_, partial);
    const bool applies = pcm ? param.pcm : param.synth;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::PushID(offset);
    ImGui::BeginDisabled(!applies);
    const float width = ImGui::GetContentRegionAvail().x;
    switch (param.format) {
    case Tone::Format::OnOff:
        if (ImGui::Button(value ? "On" : "Off", ImVec2(width, 0.0f))) {
            set(host, offset, value ? 0 : 1);
            focus_ = partial;
        }
        break;
    case Tone::Format::Waveform:
        if (ImGui::Button((value & 1) ? "SAW" : "SQU", ImVec2(width, 0.0f))) {
            set(host, offset, value ^ 1);
            focus_ = partial;
        }
        UiStyle::setItemTooltip("Square or sawtooth: click to switch");
        break;
    case Tone::Format::Wave: {
        const int bank = (tone_[size_t(base + Tone::Partial::Waveform)] >> 1) & 1;
        const std::string text = Tone::formatValue(model_, param, value, bank);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button(text.c_str(), ImVec2(width, 0.0f))) {
            ImGui::OpenPopup("waves");
            waveFilter_.clear();
            focus_ = partial;
        }
        ImGui::PopStyleVar();
        UiStyle::setItemTooltip("%s", text.c_str());
        drawWavePopup(host, partial);
        break;
    }
    default: {
        int edited = value;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string text = formatText(Tone::formatValue(model_, param, value));
        const bool changed = ImGui::SliderInt("##value", &edited, 0, param.max, text.c_str(), ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            beginStep();
            focus_ = partial;
        }
        if (changed) set(host, offset, edited);
        if (io.KeyCtrl) {
            if (const int steps = UiStyle::wheelSteps()) {
                set(host, offset, value + steps * (io.KeyShift ? 10 : 1));
                focus_ = partial;
            }
        }
        break;
    }
    }
    ImGui::EndDisabled();
    if (applies && ImGui::BeginPopupContextItem("cell")) {
        const int bank = (tone_[size_t(base + Tone::Partial::Waveform)] >> 1) & 1;
        const std::string shown = Tone::formatValue(model_, param, value, bank);
        if (ImGui::MenuItem(("Set all partials to " + shown).c_str())) {
            beginStep();
            for (int other = 0; other < Tone::kPartials; other++) {
                const int otherBase = Tone::partialBase(other);
                tone_[size_t(otherBase + param.offset)] = uint8_t(value);
                if (param.format == Tone::Format::Wave) {
                    uint8_t& form = tone_[size_t(otherBase + Tone::Partial::Waveform)];
                    form = uint8_t((form & 1) | (bank << 1));
                }
            }
            sync(host);
        }
        const std::string initial = Tone::formatValue(model_, param, param.initial, 0);
        if (ImGui::MenuItem(("Initial value: " + initial).c_str())) set(host, offset, param.initial);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void ToneEditor::drawWavePopup(Host& host, int partial) {
    if (!ImGui::BeginPopup("waves")) return;
    const float fs = ImGui::GetFontSize();
    const int base = Tone::partialBase(partial);
    const int bank = (tone_[size_t(base + Tone::Partial::Waveform)] >> 1) & 1;
    const int number = tone_[size_t(base + Tone::Partial::PcmWave)];
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(fs * 16.0f);
    char filter[64];
    std::snprintf(filter, sizeof(filter), "%s", waveFilter_.c_str());
    if (ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter))) waveFilter_ = filter;
    std::string needle = waveFilter_;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    ImGui::BeginChild("list", ImVec2(fs * 16.0f, fs * 22.0f));
    for (int b = 0; b < 2; b++) {
        ImGui::SeparatorText(b == 0 ? "Bank 1" : "Bank 2");
        for (int n = 0; n < 128; n++) {
            char label[64];
            std::snprintf(label, sizeof(label), "%d-%03d %s", b + 1, n + 1, Tone::waveName(model_, b, n));
            if (!needle.empty()) {
                std::string haystack = label;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (haystack.find(needle) == std::string::npos) continue;
            }
            const bool selected = b == bank && n == number;
            if (ImGui::Selectable(label, selected)) {
                beginStep();
                uint8_t& form = tone_[size_t(base + Tone::Partial::Waveform)];
                form = uint8_t((form & 1) | (b << 1));
                tone_[size_t(base + Tone::Partial::PcmWave)] = uint8_t(n);
                sync(host);
                ImGui::CloseCurrentPopup();
            }
            if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.3f);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Samples with * are not tuned by Master Tune.");
    ImGui::EndPopup();
}

void ToneEditor::drawAudition(Host& host, double now) {
    const float fs = ImGui::GetFontSize();
    if (ImGui::Button("Play")) {
        if (timedNote_ >= 0) stopNote(host, timedNote_);
        timedNote_ = audition_.key;
        playNote(host, timedNote_, audition_.velocity);
        timedOff_ = now + audition_.lengthMs / 1000.0;
    }
    UiStyle::setItemTooltip("Plays the audition note once");
    ImGui::SameLine();
    ImGui::Checkbox("Hold", &audition_.hold);
    UiStyle::setItemTooltip("Holds the audition note down while you edit");
    ImGui::SameLine();
    if (ImGui::Checkbox("Repeat", &audition_.repeat) && audition_.repeat) nextRepeat_ = now;
    UiStyle::setItemTooltip("Plays the audition note over and over, each time with the tone as it is then");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 4.2f);
    ImGui::SliderInt("##repeat", &audition_.repeatMs, 200, 4000, "%d ms", ImGuiSliderFlags_AlwaysClamp);
    UiStyle::setItemTooltip("How often Repeat plays the note");
    ImGui::SameLine(0.0f, fs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Key");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 3.0f);
    const std::string keyText = Tone::noteName(audition_.key);
    ImGui::SliderInt("##key", &audition_.key, 24, 108, keyText.c_str(), ImGuiSliderFlags_AlwaysClamp);
    UiStyle::setItemTooltip("The audition note (or right-click a key below)");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Vel");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 3.0f);
    ImGui::SliderInt("##velocity", &audition_.velocity, 1, 127, "%d", ImGuiSliderFlags_AlwaysClamp);
    UiStyle::setItemTooltip("Velocity of the audition note and the keyboard below");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Length");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 4.2f);
    ImGui::SliderInt("##length", &audition_.lengthMs, 50, 4000, "%d ms", ImGuiSliderFlags_AlwaysClamp);
    UiStyle::setItemTooltip("How long Play and Repeat hold the note");
    ImGui::SameLine(0.0f, fs);
    ImGui::Checkbox("Restrike", &audition_.restrike);
    UiStyle::setItemTooltip("Plays held notes again after each change (the unit applies a change from the next note on)");

    int hit = -1;  // Without the keys, a note they held is let go
    if (pianoShown_) {
        std::array<bool, 128> lit = {};
        host.soundingNotes(lit);
        std::array<bool, 128> pressed = {};
        for (int key = 0; key < 128; key++) pressed[size_t(key)] = held_[size_t(key)] > 0;
        hit = Piano::draw("keys", 24, 108, fs * 3.2f, fs * 1.6f, lit, pressed);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // The key under the pointer becomes the audition note.
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            int whites = 0;
            for (int key = 24; key <= 108; key++) whites += Piano::isBlackKey(key) ? 0 : 1;
            const int white = int((ImGui::GetMousePos().x - min.x) / ((max.x - min.x) / float(whites)));
            for (int key = 24, index = 0; key <= 108; key++) {
                if (Piano::isBlackKey(key)) continue;
                if (index++ == white) audition_.key = key;
            }
        }
    }
    if (hit != mouseNote_) {
        const int old = mouseNote_;
        mouseNote_ = hit;
        if (old >= 0 && old != timedNote_ && old != holdKey_) {
            host.noteOff(old);
            held_[size_t(old)] = 0;
        }
        if (hit >= 0) playNote(host, hit, audition_.velocity);
    }
}
