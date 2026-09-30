// Headless UI snapshot: runs the real App for a number of frames without a window or GPU, then
// rasterises Dear ImGui's draw lists into a PNG. Lets the interface be checked on machines without
// a display; the emulator itself targets Windows (MainWin32.cpp).

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "App.h"
#include "PatternCaptureApp.h"
#include "Platform.h"
#include "RolandSysex.h"
#include "RomLibrary.h"
#include "ToneEditorApp.h"
#include "TranslatorApp.h"
#include "UiStyle.h"
#include "imgui.h"

namespace {

// Dialogs are cancelled at once (a click on a file button starts no zenity on a desktop), and key names are the
// built-in ones, as without a window system.
class NoDialogs : public Platform::Toolkit {
public:
    bool openFileDialog(Platform::FileKind, std::filesystem::path&) override { return false; }
    bool saveFileDialog(Platform::FileKind, const std::string&, std::filesystem::path&) override { return false; }
    bool pickFolderDialog(const std::filesystem::path&, std::filesystem::path&) override { return false; }
    std::string takeDialogError() override { return std::string(); }
    std::string keyName(int) override { return std::string(); }
};

void printUsage() {
    std::fprintf(stderr,
                 "Usage: uisnap [options] output.png\n"
                 "  --size WxH        Window size (default 1200x780)\n"
                 "  --midi FILE       MIDI file to play before taking the snapshot\n"
                 "  --seconds S       Song time to play before the snapshot (default 3)\n"
                 "  --roms DIR        ROM folder\n"
                 "  --scale F         UI scale, like a high-DPI monitor (default 1)\n"
                 "  --syx FILE        SysEx file to load first (may be repeated)\n"
                 "  --rom-song N      Start ROM Play song N (1-8, 0 = all)\n"
                 "  --d20 P           Play D-20 rhythm pattern P (P-11 to P-88) or the rhythm track (track)\n"
                 "  --d20-next P      With a --d20 pattern: after 1 s, choose pattern P, which plays from the next bar\n"
                 "  --lcd-tones       D-20 performance mode: the LCD shows the upper and lower tones (DISPLAY)\n"
                 "  --reverb          The Reverb tuning window\n"
                 "  --mt32-presets    The MT-32 presets window\n"
                 "  --config          The configuration window (File > Configuration...: ROMs, audio, MIDI input)\n"
                 "  --no-keyboard     View > Keyboard off (no keyboard under the Play and Performance tabs, no audition keys)\n"
                 "  --no-partials     View > Partials off (no partial display in the Play tab)\n"
                 "  --plugin          D110Emu as the VST2 plugin shows itself (no audio devices, MIDI ports or Exit;\n"
                 "                    View > Window size)\n"
                 "  --vst3            As the VST3 plugin (--plugin: the VST2 one)\n"
                 "  --au              As the Audio Unit (MULTI 1-6 always as stereo pairs)\n"
                 "  --multi-pairs     MULTI 1-6 as stereo pairs: the standalone's choice (heard with --surround), or with\n"
                 "                    --plugin or --vst3 the instance's; --multi-pairs-new: a plugin's choice for new\n"
                 "                    instances only (the configuration window's note; implies --plugin)\n"
                 "  --own-outputs M   The parts in mask M on their own outputs (bit n = part number n, 8 = rhythm;\n"
                 "                    0x101: part 1 and rhythm); implies --plugin\n"
                 "  --host-outputs M  The outputs the VST3 host takes (bit n = part number n's own, bit 16 + n = MULTI n + 1;\n"
                 "                    default all); implies --vst3\n"
                 "  --patterncapture  PatternCapture's window, fed a made-up D-20 (two takes stored, a third recording);\n"
                 "                    --syx FILE opens a dump there first\n"
                 "  --click X,Y[,T]   Click the left mouse button at X,Y after T seconds (default 1; may be repeated)\n"
                 "  --hover X,Y[,T]   Move the pointer to X,Y after T seconds and leave it there (tooltips)\n"
                 "  --right-click X,Y[,T]  The same with the right button\n"
                 "  --preset-mode M   With --mt32: MT-32 presets as standins, hybrid or exact (the default)\n"
                 "  --lcd-scheme N    The LCD's colours (0 D-110, 1 D-10/D-20, ...)\n"
                 "  --lcd-colours G,U,L  The user's own LCD colours: glass, unlit and lit dots as RRGGBB (selects them)\n"
                 "  --lcd-editor      The Custom LCD colours window\n"
                 "  --tab NAME        Tab to show: Play, Parts, Tone, Timbres, Patches, Performance, Rhythm, Patterns or System\n"
                 "  --part N          Part shown in the editor and on the LCD (1-15, R = rhythm)\n"
                 "  --sixteen         16-part mode\n"
                 "  --performance     D-20 performance mode\n"
                 "  --mt32            MT-32 translation\n"
                 "  --nice-panning    Nice panning (pan sliders from -64 to +64)\n"
                 "  --surround        Audio output in 7.1 surround (the MULTI outputs on speakers of their own)\n"
                 "  --card FILE       Memory card file to insert\n"
                 "  --memory FILE     Battery memory file, loaded at start and saved on exit\n"
                 "  --translator      MT32Translator's window instead (--syx files go through its pipe)\n"
                 "  --target d110|d20 MT32Translator's unit (--tab Presets shows its preset editor)\n"
                 "  --cache           MT32Translator's tone cache on\n"
                 "  --toneeditor      ToneEditor's window instead (--target d110|d20|mt32; --syx files go to its library,\n"
                 "                    a file with one tone into the editor; --part N edits part N; --tab Tone, Parts,\n"
                 "                    Timbres, Performance, Rhythm or System; --performance: the D-20's performance mode;\n"
                 "                    --unit connects it to d110emu's engine with the D-110 ROMs, which answers its\n"
                 "                    requests like a real unit)\n");
}

// Keeps Dear ImGui's texture requests satisfied; the pixels stay owned by ImTextureData.
void updateTextures(ImDrawData* drawData) {
    if (drawData->Textures == nullptr) return;
    for (ImTextureData* texture : *drawData->Textures) {
        switch (texture->Status) {
        case ImTextureStatus_WantCreate:
        case ImTextureStatus_WantUpdates:
            texture->SetTexID(ImTextureID(reinterpret_cast<intptr_t>(texture)));
            texture->SetStatus(ImTextureStatus_OK);
            break;
        case ImTextureStatus_WantDestroy:
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
            break;
        default:
            break;
        }
    }
}

struct Canvas {
    int width;
    int height;
    std::vector<float> rgb;  // Linear blend buffer, 0..1

    Canvas(int w, int h, const ImVec4& clear) : width(w), height(h), rgb(size_t(w) * h * 3) {
        for (size_t i = 0; i < rgb.size(); i += 3) {
            rgb[i] = clear.x;
            rgb[i + 1] = clear.y;
            rgb[i + 2] = clear.z;
        }
    }
};

void sampleTexture(const ImTextureData* texture, float u, float v, float out[4]) {
    const int x = std::clamp(int(u * texture->Width), 0, texture->Width - 1);
    const int y = std::clamp(int(v * texture->Height), 0, texture->Height - 1);
    const unsigned char* pixel = texture->Pixels + (size_t(y) * texture->Width + x) * texture->BytesPerPixel;
    if (texture->Format == ImTextureFormat_Alpha8) {
        out[0] = out[1] = out[2] = 1.0f;
        out[3] = pixel[0] / 255.0f;
    } else {
        for (int c = 0; c < 4; c++) out[c] = pixel[c] / 255.0f;
    }
}

void drawTriangle(Canvas& canvas, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImVec4& clip,
                  const ImTextureData* texture) {
    float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (std::fabs(area) < 1e-6f) return;
    const int minX = std::max(int(std::floor(std::max(std::min({a.pos.x, b.pos.x, c.pos.x}), clip.x))), 0);
    const int minY = std::max(int(std::floor(std::max(std::min({a.pos.y, b.pos.y, c.pos.y}), clip.y))), 0);
    const int maxX = std::min(int(std::ceil(std::min(std::max({a.pos.x, b.pos.x, c.pos.x}), clip.z))), canvas.width);
    const int maxY = std::min(int(std::ceil(std::min(std::max({a.pos.y, b.pos.y, c.pos.y}), clip.w))), canvas.height);
    const ImVec4 colorA = ImGui::ColorConvertU32ToFloat4(a.col);
    const ImVec4 colorB = ImGui::ColorConvertU32ToFloat4(b.col);
    const ImVec4 colorC = ImGui::ColorConvertU32ToFloat4(c.col);
    for (int y = minY; y < maxY; y++) {
        for (int x = minX; x < maxX; x++) {
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            float w0 = (b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px);
            float w1 = (c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px);
            float w2 = (a.pos.x - px) * (b.pos.y - py) - (a.pos.y - py) * (b.pos.x - px);
            w0 /= area;
            w1 /= area;
            w2 /= area;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            float texel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (texture != nullptr) {
                sampleTexture(texture, w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y, texel);
            }
            const float red = texel[0] * (w0 * colorA.x + w1 * colorB.x + w2 * colorC.x);
            const float green = texel[1] * (w0 * colorA.y + w1 * colorB.y + w2 * colorC.y);
            const float blue = texel[2] * (w0 * colorA.z + w1 * colorB.z + w2 * colorC.z);
            const float alpha = texel[3] * (w0 * colorA.w + w1 * colorB.w + w2 * colorC.w);
            float* dst = &canvas.rgb[(size_t(y) * canvas.width + x) * 3];
            dst[0] = red * alpha + dst[0] * (1.0f - alpha);
            dst[1] = green * alpha + dst[1] * (1.0f - alpha);
            dst[2] = blue * alpha + dst[2] * (1.0f - alpha);
        }
    }
}

void rasterize(ImDrawData* drawData, Canvas& canvas) {
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) continue;
            const ImVec4 clip(cmd.ClipRect.x - drawData->DisplayPos.x, cmd.ClipRect.y - drawData->DisplayPos.y,
                              cmd.ClipRect.z - drawData->DisplayPos.x, cmd.ClipRect.w - drawData->DisplayPos.y);
            const ImTextureData* texture = reinterpret_cast<const ImTextureData*>(intptr_t(cmd.GetTexID()));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawIdx* idx = &list->IdxBuffer[int(cmd.IdxOffset + i)];
                const ImDrawVert* vtx = &list->VtxBuffer[int(cmd.VtxOffset)];
                drawTriangle(canvas, vtx[idx[0]], vtx[idx[1]], vtx[idx[2]], clip, texture);
            }
        }
    }
}

uint32_t crc32(const std::vector<uint8_t>& data, size_t start) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = start; i < data.size(); i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Uncompressed PNG (zlib "stored" blocks): large but needs no compression library.
bool writePng(const std::filesystem::path& path, const Canvas& canvas) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(canvas.width) * 3 + 1) * canvas.height);
    for (int y = 0; y < canvas.height; y++) {
        raw.push_back(0);  // Filter: none
        for (int x = 0; x < canvas.width * 3; x++) {
            const float value = canvas.rgb[size_t(y) * canvas.width * 3 + x];
            raw.push_back(uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)));
        }
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
        const size_t length = std::min<size_t>(65535, raw.size() - pos);
        zlib.push_back(pos + length == raw.size() ? 1 : 0);
        zlib.push_back(uint8_t(length));
        zlib.push_back(uint8_t(length >> 8));
        zlib.push_back(uint8_t(~length));
        zlib.push_back(uint8_t(~length >> 8));
        zlib.insert(zlib.end(), raw.begin() + long(pos), raw.begin() + long(pos + length));
        pos += length;
    }
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) zlib.push_back(uint8_t(adler >> shift));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
        for (int shift = 24; shift >= 0; shift -= 8) png.push_back(uint8_t(body.size() >> shift));
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), body.begin(), body.end());
        const uint32_t crc = crc32(png, start);
        for (int shift = 24; shift >= 0; shift -= 8) png.push_back(uint8_t(crc >> shift));
    };
    std::vector<uint8_t> header;
    for (uint32_t value : {uint32_t(canvas.width), uint32_t(canvas.height)}) {
        for (int shift = 24; shift >= 0; shift -= 8) header.push_back(uint8_t(value >> shift));
    }
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    chunk("IHDR", header);
    chunk("IDAT", zlib);
    chunk("IEND", {});

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(out);
}

}  // namespace

int main(int argc, char** argv) {
    static NoDialogs noDialogs;
    Platform::setToolkit(&noDialogs);
    int width = 1200;
    int height = 780;
    float scale = 1.0f;
    double seconds = 3.0;
    std::filesystem::path midiFile;
    std::filesystem::path romFolder;
    std::vector<std::filesystem::path> sysexFiles;
    int romSong = -1;
    int d20 = -1;  // D-20 pattern 0-63, or kD20PatternCount for the rhythm track
    int d20Next = -1;  // D-20 pattern chosen while --d20's repeats
    bool lcdTones = false;
    bool reverbWindow = false;
    bool presetsWindow = false;
    bool configWindow = false;
    bool plugin = false;
    bool vst3 = false;
    bool au = false;
    bool multiPairs = false;     // This instance's MULTI layout
    bool multiPairsNew = false;  // The choice for new instances
    long ownOutputs = -1;
    uint32_t hostOutputs = 0x3FFFFF;  // All: the parts' own outputs and MULTI 1-6
    bool noKeyboard = false;
    bool noPartials = false;
    bool patternCapture = false;
    std::string presetMode;
    // Mouse clicks: the pointer moves there on the click's frame, the button goes down on the next and up two later,
    // then the pointer leaves the window (so the snapshot shows no hover highlight).
    struct Click {
        float x = 0.0f;
        float y = 0.0f;
        int frame = 60;
        int button = 0;
    };
    std::vector<Click> clicks;
    int lcdScheme = -1;
    std::string lcdColours;  // "RRGGBB,RRGGBB,RRGGBB"
    bool lcdEditor = false;
    int part = 1;  // 1-16, 0 = rhythm
    bool sixteen = false;
    bool performance = false;
    bool mt32 = false;
    bool nicePanning = false;
    bool surround = false;
    std::filesystem::path cardFile;
    std::string tab;
    std::filesystem::path memoryFile;
    std::filesystem::path output;
    bool translator = false;
    bool toneEditor = false;
    bool emulatedUnit = false;
    bool cache = false;
    std::string target;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--size" && hasValue) {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2) width = 0;
        } else if (arg == "--midi" && hasValue) {
            midiFile = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--seconds" && hasValue) {
            seconds = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--roms" && hasValue) {
            romFolder = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--syx" && hasValue) {
            sysexFiles.push_back(Platform::fromUtf8(argv[++i]));
        } else if (arg == "--rom-song" && hasValue) {
            romSong = std::atoi(argv[++i]);
        } else if ((arg == "--d20" || arg == "--d20-next") && hasValue) {
            const std::string value = argv[++i];
            const std::string digits = value.size() >= 2 ? value.substr(value.size() - 2) : value;
            int& what = arg == "--d20" ? d20 : d20Next;
            if (value == "track" && arg == "--d20") {
                what = kD20PatternCount;
            } else if (digits.size() == 2 && digits[0] >= '1' && digits[0] <= '8' && digits[1] >= '1' && digits[1] <= '8') {
                what = (digits[0] - '1') * 8 + (digits[1] - '1');
            } else {
                printUsage();
                return 2;
            }
        } else if (arg == "--lcd-tones") {
            lcdTones = true;
        } else if (arg == "--reverb") {
            reverbWindow = true;
        } else if (arg == "--mt32-presets") {
            presetsWindow = true;
        } else if (arg == "--config") {
            configWindow = true;
        } else if (arg == "--plugin") {
            plugin = true;
        } else if (arg == "--vst3") {
            plugin = true;
            vst3 = true;
        } else if (arg == "--au") {
            plugin = true;
            au = true;
            multiPairs = true;
        } else if (arg == "--multi-pairs") {
            multiPairs = true;
            multiPairsNew = true;
        } else if (arg == "--multi-pairs-new") {
            plugin = true;
            multiPairsNew = true;
        } else if (arg == "--own-outputs" && i + 1 < argc) {
            plugin = true;
            ownOutputs = long(std::stoul(argv[++i], nullptr, 0));
        } else if (arg == "--host-outputs" && i + 1 < argc) {
            plugin = true;
            vst3 = true;
            hostOutputs = uint32_t(std::stoul(argv[++i], nullptr, 0));
        } else if (arg == "--no-keyboard") {
            noKeyboard = true;
        } else if (arg == "--no-partials") {
            noPartials = true;
        } else if (arg == "--patterncapture") {
            patternCapture = true;
        } else if ((arg == "--click" || arg == "--right-click" || arg == "--hover") && hasValue) {
            Click click;
            float at = 1.0f;
            if (std::sscanf(argv[++i], "%f,%f,%f", &click.x, &click.y, &at) < 2) {
                printUsage();
                return 2;
            }
            click.frame = std::max(1, int(at * 60.0f));
            click.button = arg == "--click" ? 0 : arg == "--right-click" ? 1 : -1;  // -1: no button
            clicks.push_back(click);
        } else if (arg == "--preset-mode" && hasValue) {
            presetMode = argv[++i];
        } else if (arg == "--lcd-scheme" && hasValue) {
            lcdScheme = std::atoi(argv[++i]);
        } else if (arg == "--lcd-colours" && hasValue) {
            lcdColours = argv[++i];
        } else if (arg == "--lcd-editor") {
            lcdEditor = true;
        } else if (arg == "--memory" && hasValue) {
            memoryFile = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--tab" && hasValue) {
            tab = argv[++i];
        } else if (arg == "--part" && hasValue) {
            const std::string value = argv[++i];
            part = value == "R" || value == "r" ? 0 : std::clamp(std::atoi(value.c_str()), 1, 15);
        } else if (arg == "--sixteen") {
            sixteen = true;
        } else if (arg == "--performance") {
            performance = true;
        } else if (arg == "--mt32") {
            mt32 = true;
        } else if (arg == "--toneeditor") {
            toneEditor = true;
        } else if (arg == "--unit") {
            emulatedUnit = true;
        } else if (arg == "--translator") {
            translator = true;
        } else if (arg == "--cache") {
            cache = true;
        } else if (arg == "--target" && hasValue) {
            target = argv[++i];
        } else if (arg == "--surround") {
            surround = true;
        } else if (arg == "--nice-panning") {
            nicePanning = true;
        } else if (arg == "--card" && hasValue) {
            cardFile = std::filesystem::absolute(Platform::fromUtf8(argv[++i]));
        } else if (arg == "--scale" && hasValue) {
            scale = std::clamp(float(std::atof(argv[++i])), 0.5f, 4.0f);
        } else if (arg[0] != '-' && output.empty()) {
            output = Platform::fromUtf8(arg);
        } else {
            printUsage();
            return 2;
        }
    }
    if (output.empty() || width < 200 || height < 200) {
        printUsage();
        return 2;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // rasterize() honours ImDrawCmd::VtxOffset
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = 1.0f / 60.0f;
    App::applyStyle();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui::GetStyle().FontSizeBase = 16.0f;
    io.Fonts->AddFontDefault();

    if (toneEditor) {
        // ToneEditor: its link runs, without MIDI ports.
        const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "toneeditor-uisnap.ini";
        std::filesystem::remove(settingsFile);
        {
            std::ofstream seed(settingsFile);
            seed << "model = " << (target.empty() ? std::string("d20") : target) << "\n";
            seed << "part = " << std::clamp(part, 1, 8) << "\n";
            if (performance) seed << "performance_mode = true\n";
        }
        ToneEditorApp app;
        ToneEditorOptions options;
        options.settingsFile = settingsFile;
        options.enableMidi = false;
        app.init(options);
        // --unit: d110emu's engine plays the unit, answering data requests (RQ1) from its memory as a real D-110 does.
        struct EngineUnit : MidiSender {
            SynthEngine engine;
            UnitLink* link = nullptr;
            bool sendShort(uint32_t message) override {
                engine.onMidiShortMessage(message);
                return true;
            }
            bool sendSysex(const uint8_t* data, size_t length) override {
                if (length == 13 && data[4] == 0x11) {
                    // A unit takes messages in order: what came before the request is in memory when it answers.
                    std::vector<float> audio(2 * 32);
                    engine.render(audio.data(), 32);
                    const uint32_t address = uint32_t(data[5]) << 14 | uint32_t(data[6]) << 7 | data[7];
                    const uint32_t size = uint32_t(data[8]) << 14 | uint32_t(data[9]) << 7 | data[10];
                    std::vector<uint8_t> bytes(size);
                    engine.readMemory(RolandSysex::unpack(address), size, bytes.data());
                    for (uint32_t offset = 0; offset < size; offset += 256) {  // At most 256 bytes a message, as the units send
                        const uint32_t length256 = std::min<uint32_t>(256, size - offset);
                        const std::vector<uint8_t> reply = RolandSysex::dataSet(data[2], address + offset, bytes.data() + offset, length256);
                        link->onMidiSysex(reply.data(), reply.size());
                    }
                } else {
                    engine.onMidiSysex(data, length);
                }
                return true;
            }
        };
        std::unique_ptr<EngineUnit> unit;
        if (emulatedUnit) {
            unit.reset(new EngineUnit);
            EngineOptions engineOptions;
            engineOptions.midiTimestamping = false;
            unit->engine.setOptions(engineOptions);
            EngineConfig config;
            const std::vector<RomEntry> roms =
                scanRomFolder(romFolder.empty() ? findRomFolder({Platform::executableDirectory(), std::filesystem::current_path()}) : romFolder);
            int control = -1, pcm = -1;
            std::string error;
            if (pickDefaultRoms(roms, control, pcm)) {
                config.controlRom = roms[size_t(control)].path;
                config.pcmRom = roms[size_t(pcm)].path;
            }
            if (!unit->engine.configure(config, error)) {
                std::fprintf(stderr, "--unit: %s\n", error.c_str());
                return 2;
            }
            for (const std::filesystem::path& file : sysexFiles) unit->engine.loadSysexFile(file, error, false);
            if (performance) unit->engine.setPerformanceMode(true, 0);
            unit->link = &app.link();
            app.link().setOutput(unit.get());
            app.requestPartTone();
            app.requestSetup();
            if (performance || tab == "Performance") app.requestPatches();
            app.requestLibrary();
        } else {
            for (const std::filesystem::path& file : sysexFiles) app.openFile(file);
        }
        if (!tab.empty()) app.showTab(tab);
        const int frames = std::max(3, int(seconds * 60.0));
        UnitLink::Clock::time_point virtualTime = UnitLink::Clock::now() + std::chrono::hours(1);
        for (int frame = 0; frame < frames; frame++) {
            for (UnitLink::Clock::time_point due = virtualTime; due != UnitLink::Clock::time_point::max(); due = app.link().process(due)) {
                virtualTime = std::max(virtualTime, due);
            }
            if (unit) {
                std::vector<float> audio(2 * 800);
                unit->engine.render(audio.data(), 800);  // The unit takes in what it was sent
            }
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            updateTextures(ImGui::GetDrawData());
        }
        Canvas canvas(width, height, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
        rasterize(ImGui::GetDrawData(), canvas);
        const bool written = writePng(output, canvas);
        const UnitLink::Stats stats = app.link().stats();
        std::printf("link: sent %llu (%llu SysEx bytes), merged %llu; tone \"%s\"\n", static_cast<unsigned long long>(stats.sent),
                    static_cast<unsigned long long>(stats.sysexBytes), static_cast<unsigned long long>(stats.merged),
                    Tone::name(app.editor().tone()).c_str());
        app.shutdown();
        ImGui::DestroyContext();
        std::filesystem::remove(settingsFile);
        if (!written) {
            std::fprintf(stderr, "Cannot write %s\n", Platform::toUtf8(output).c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", Platform::toUtf8(output).c_str(), width, height);
        return 0;
    }

    if (patternCapture) {
        // PatternCapture, fed what a D-20 sends in Pattern Play (Start, 24 clocks per quarter at 120 BPM, the notes on
        // channel 10 half a millisecond after their clock, Stop): a 4/4 beat and a waltz, then a pattern still playing.
        const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "patterncapture-uisnap.ini";
        const std::filesystem::path sessionFile = std::filesystem::temp_directory_path() / "patterncapture-uisnap.syx";
        std::filesystem::remove(settingsFile);
        std::filesystem::remove(sessionFile);
        PatternCaptureApp app;
        PatternCaptureOptions options;
        options.settingsFile = settingsFile;
        options.sessionFile = sessionFile;
        options.enableMidi = false;
        app.init(options);
        for (const std::filesystem::path& file : sysexFiles) app.openFile(file);
        auto makePattern = [](int beats, std::initializer_list<std::array<int, 3>> notes) {
            D20Pattern pattern;
            pattern.beats = beats;
            pattern.present = true;
            for (const auto& n : notes) pattern.notes.push_back({n[0], n[1], n[2]});
            return pattern;
        };
        const D20Pattern beat = makePattern(4, {{0, 36, 120}, {0, 42, 100}, {12, 42, 70}, {24, 38, 110}, {24, 42, 100}, {36, 42, 70},
                                                 {48, 36, 120}, {48, 42, 100}, {60, 42, 70}, {66, 36, 90}, {72, 38, 110}, {72, 42, 100},
                                                 {84, 42, 70}, {90, 38, 60}});
        const D20Pattern waltz = makePattern(3, {{0, 36, 110}, {0, 51, 100}, {24, 51, 80}, {24, 42, 90}, {36, 51, 60}, {48, 51, 80},
                                                  {48, 42, 90}});
        const D20Pattern sixteen = makePattern(4, {{0, 36, 127}, {0, 49, 110}, {6, 42, 60}, {12, 42, 90}, {18, 42, 60}, {24, 38, 115},
                                                    {30, 42, 60}, {36, 36, 100}, {42, 42, 60}, {48, 36, 120}, {54, 42, 60},
                                                    {60, 42, 90}, {66, 42, 60}, {72, 38, 115}, {78, 42, 60}, {84, 42, 90}, {90, 38, 70}});
        double time = 1000.0;
        auto play = [&](const D20Pattern& pattern, int steps, bool stop) {
            const double clock = 60.0 / 120.0 / kD20StepsPerQuarter;
            const int length = pattern.beats * kD20StepsPerQuarter;
            app.receive(0xFA, time);
            for (int step = 0; step < steps; step++) {
                app.receive(0xF8, time);
                for (const D20Pattern::Note& note : pattern.notes) {
                    if (note.step == step % length) app.receive(0x99u | uint32_t(note.key) << 8 | uint32_t(note.velocity) << 16, time + 0.0005);
                }
                time += clock;
            }
            if (stop) app.receive(0xFC, time);
            time += 1.0;
        };
        play(beat, 4 * 96, true);
        play(waltz, 4 * 72, true);
        play(sixteen, 96 + 60, false);
        const int frames = std::max(3, int(seconds * 60.0));
        for (int frame = 0; frame < frames; frame++) {
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            updateTextures(ImGui::GetDrawData());
        }
        Canvas canvas(width, height, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
        rasterize(ImGui::GetDrawData(), canvas);
        const bool written = writePng(output, canvas);
        for (int slot = 0; slot < kD20PresetPatterns; slot++) {
            const D20Pattern& pattern = app.patterns()[size_t(slot)];
            if (pattern.present) std::printf("  %s: %d/4, %zu notes\n", d20PatternName(slot).c_str(), pattern.beats, pattern.notes.size());
        }
        std::printf("next slot: %s\n", d20PatternName(app.slot()).c_str());
        app.shutdown();
        ImGui::DestroyContext();
        std::filesystem::remove(settingsFile);
        std::filesystem::remove(sessionFile);
        if (!written) {
            std::fprintf(stderr, "Cannot write %s\n", Platform::toUtf8(output).c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", Platform::toUtf8(output).c_str(), width, height);
        return 0;
    }

    if (translator) {
        // MT32Translator: its pipe runs, without MIDI ports; SysEx files go through it.
        const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "mt32translator-uisnap.ini";
        std::filesystem::remove(settingsFile);
        {
            std::ofstream seed(settingsFile);
            if (!target.empty()) seed << "target = " << target << "\n";
            if (cache) seed << "cache_enabled = true\n";
        }
        TranslatorApp app;
        TranslatorOptions options;
        options.settingsFile = settingsFile;
        options.romSearchDirs = {romFolder.empty() ? Platform::executableDirectory() : std::filesystem::absolute(romFolder).parent_path(),
                                 std::filesystem::current_path()};
        options.enableMidi = false;
        app.init(options);
        for (const std::filesystem::path& file : sysexFiles) app.openFile(file);
        if (tab == "Presets") app.showPresetsTab();
        const int frames = std::max(3, int(seconds * 60.0));
        MidiPipe::Clock::time_point virtualTime = MidiPipe::Clock::now() + std::chrono::hours(1);
        for (int frame = 0; frame < frames; frame++) {
            // Everything queued is sent at once, on a clock of its own (the pipe's thread then has nothing due).
            for (MidiPipe::Clock::time_point due = virtualTime; due != MidiPipe::Clock::time_point::max(); due = app.pipe().process(due)) {
                virtualTime = std::max(virtualTime, due);
            }
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            updateTextures(ImGui::GetDrawData());
        }
        Canvas canvas(width, height, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
        rasterize(ImGui::GetDrawData(), canvas);
        const bool written = writePng(output, canvas);
        const MidiPipe::Stats stats = app.pipe().stats();
        std::printf("pipe: received %llu, sent %llu (%llu SysEx bytes), replies %llu\n", static_cast<unsigned long long>(stats.received),
                    static_cast<unsigned long long>(stats.sent), static_cast<unsigned long long>(stats.sysexBytesSent),
                    static_cast<unsigned long long>(stats.replies));
        app.shutdown();
        ImGui::DestroyContext();
        std::filesystem::remove(settingsFile);
        if (!written) {
            std::fprintf(stderr, "Cannot write %s\n", Platform::toUtf8(output).c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", Platform::toUtf8(output).c_str(), width, height);
        return 0;
    }

    // A throwaway settings file, pre-seeded with the ROM folder if one was given.
    const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "d110emu-uisnap.ini";
    std::filesystem::remove(settingsFile);
    {
        std::ofstream seed(settingsFile);
        if (!romFolder.empty()) seed << "rom_folder = " << Platform::toUtf8(std::filesystem::absolute(romFolder)) << "\n";
        if (sixteen) seed << "sixteen_parts = true\npartials = 512\n";
        if (performance) seed << "performance_mode = true\n";
        if (mt32) seed << "mt32_translation = true\n";
        if (!presetMode.empty()) seed << "mt32_preset_mode = " << presetMode << "\n";
        if (nicePanning) seed << "nice_panning = true\n";
        if (surround) seed << "audio_channels = 7.1\n";
        if (multiPairs && !plugin) seed << "multi_output_pairs = true\n";
        if (lcdScheme >= 0) seed << "lcd_scheme = " << lcdScheme << "\n";
        if (!lcdColours.empty()) {
            static const char* const keys[3] = {"lcd_custom_glass", "lcd_custom_dot_off", "lcd_custom_dot_on"};
            seed << "lcd_scheme = custom\n";
            std::stringstream list(lcdColours);
            std::string colour;
            for (int k = 0; k < 3 && std::getline(list, colour, ','); k++) seed << keys[k] << " = #" << colour << "\n";
        }
        if (noKeyboard) seed << "show_keyboard = false\n";
        if (noPartials) seed << "show_partials = false\n";
        if (ownOutputs >= 0) seed << "own_outputs = " << ownOutputs << "\n";
        if (!cardFile.empty()) seed << "card_file = " << Platform::toUtf8(cardFile) << "\n";
    }

    App app;
    AppOptions options;
    options.settingsFile = settingsFile;
    options.memoryFile = memoryFile;
    options.romSearchDirs = {Platform::executableDirectory(), std::filesystem::current_path()};
    options.enableAudio = false;
    options.enableMidiInput = false;
    if (plugin) {
        options.plugin = true;
        options.partOutputs = true;
        options.pluginFormat = au ? "AU" : vst3 ? "VST3" : "VST2";
        if (vst3) options.hostOutputs = [hostOutputs] { return hostOutputs; };
        options.multiOutputPairs = multiPairs;
        if (!au) {
            // As PluginCore keeps it: the choice for new instances, here only in memory.
            options.multiPairsForNew = [&multiPairsNew] { return multiPairsNew; };
            options.setMultiPairsForNew = [&multiPairsNew](bool pairs) { multiPairsNew = pairs; };
        }
        options.outputSampleRate = 48000;
        options.reverbSettingsFile = std::filesystem::temp_directory_path() / "d110emu-uisnap-reverb.ini";
        // As the plugin's editor draws them (PluginEditorWin32.cpp).
        options.viewMenu = [] {
            if (!ImGui::BeginMenu("Window size")) return;
            for (int zoom : {75, 100, 125, 150, 200}) ImGui::MenuItem((std::to_string(zoom) + "%").c_str(), nullptr, zoom == 100);
            ImGui::EndMenu();
        };
    }
    app.init(options);
    for (const std::filesystem::path& file : sysexFiles) app.openFile(file);
    if (!midiFile.empty()) app.openFile(midiFile);
    if (romSong >= 0) app.playRomSong(romSong - 1);
    app.setEditPart(part == 0 ? kRhythmPart : (part <= 8 ? part - 1 : part));
    app.showLcdTones(lcdTones);
    app.showReverbTuning(reverbWindow);
    app.showMt32Presets(presetsWindow);
    if (configWindow) app.showConfiguration(true);  // It also opens by itself when the synth cannot start
    if (lcdEditor) app.showLcdColourEditor(true);

    // Advance the synth in 60 fps steps (800 frames at 48 kHz), drawing a UI frame after each one.
    std::vector<float> audio(2 * 800);
    const int frames = std::max(3, int(seconds * 60.0));
    for (int frame = 0; frame < frames; frame++) {
        app.engine().render(audio.data(), 800);
        if (frame == 0 && !tab.empty()) app.selectTab(tab);
        if (frame == 1 && d20 >= 0) app.playD20Rhythm(d20);  // After a frame has read the patterns
        if (frame == 61 && d20Next >= 0) app.queueD20Pattern(d20Next);
        for (const Click& click : clicks) {
            if (frame == click.frame) io.AddMousePosEvent(click.x, click.y);
            if (click.button < 0) continue;  // A hover: the pointer stays
            if (frame == click.frame + 1) io.AddMouseButtonEvent(click.button, true);
            if (frame == click.frame + 3) io.AddMouseButtonEvent(click.button, false);
            if (frame == click.frame + 5) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        updateTextures(ImGui::GetDrawData());
    }

    Canvas canvas(width, height, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
    rasterize(ImGui::GetDrawData(), canvas);
    const bool written = writePng(output, canvas);

    EngineStatus status;
    app.engine().getStatus(status);
    std::printf("synth %s: %s + %s\n", status.open ? "open" : "closed", status.controlRomName.c_str(), status.pcmRomName.c_str());
    for (int i = 0; i < int(status.partCount); i++) {
        std::printf("  part %s ch %2s  %-3s %-10s notes %u\n", i == kRhythmPart ? "R" : std::to_string(i < kRhythmPart ? i + 1 : i).c_str(),
                    status.parts[i].channel < 16 ? std::to_string(status.parts[i].channel + 1).c_str() : "--",
                    status.parts[i].tone, status.parts[i].name, status.parts[i].noteCount);
    }
    std::printf("player: %s at %.1f / %.1f s%s%s\n", status.player.name.c_str(), status.player.position, status.player.duration,
                status.player.queuedName.empty() ? "" : ", next: ", status.player.queuedName.c_str());
    const std::string lcd = app.lcdText();
    std::printf("lcd: |%s|%s|\n", lcd.substr(0, 16).c_str(), lcd.substr(16).c_str());

    app.shutdown();
    ImGui::DestroyContext();
    std::filesystem::remove(settingsFile);
    if (!written) {
        std::fprintf(stderr, "Cannot write %s\n", Platform::toUtf8(output).c_str());
        return 1;
    }
    std::printf("wrote %s (%dx%d)\n", Platform::toUtf8(output).c_str(), width, height);
    return 0;
}
