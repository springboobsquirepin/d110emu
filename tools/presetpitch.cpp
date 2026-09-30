// Compares MT-32 presets with their D-110 stand-ins by rendering both at key 60 (MT-32 ROMs and D-110 ROMs from the
// ROM folder): fundamental (YIN, with its aperiodicity), spectral centroid and attack. A clean octave difference in
// the fundamental that the centroid agrees with is what the stand-in table's key shift corrects; the user confirmed
// the method by ear on a real D-20.
//
//   presetpitch                     every preset against its built-in stand-in
//   presetpitch 4 44                only these presets (0-127)
//   presetpitch 29:84:12            preset 29 against D-110 tone 84 (b35) with key shift +12
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Mt32Translator.h"
#include "Platform.h"
#include "RomLibrary.h"
#include "SynthEngine.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> renderNote(SynthEngine& e, uint8_t channel, double seconds) {
    std::vector<float> buf(2 * 480);
    for (int i = 0; i < 5; i++) e.render(buf.data(), 480);  // settle
    e.onMidiShortMessage(0x90 | channel | (60u << 8) | (100u << 16));
    std::vector<double> mono;
    const int blocks = int(seconds * 100);
    for (int b = 0; b < blocks; b++) {
        e.render(buf.data(), 480);
        for (size_t i = 0; i < buf.size(); i += 2) mono.push_back(double(buf[i]) + buf[i + 1]);
    }
    e.onMidiShortMessage(0x80 | channel | (60u << 8));
    for (int i = 0; i < 100; i++) e.render(buf.data(), 480);  // release
    return mono;
}

// YIN on a window starting at `startSec`; returns Hz or 0.
double yin(const std::vector<double>& x, double startSec, double& aperiodicity) {
    const size_t start = size_t(startSec * 48000), window = 4096;
    const int minLag = 24, maxLag = 1200;  // 40 Hz - 2 kHz
    aperiodicity = 1.0;
    if (x.size() < start + window + maxLag + 2) return 0.0;
    std::vector<double> cmnd(maxLag + 2, 1.0);
    double running = 0.0;
    for (int lag = 1; lag <= maxLag + 1; lag++) {
        double d = 0.0;
        for (size_t i = 0; i < window; i++) {
            const double diff = x[start + i] - x[start + i + lag];
            d += diff * diff;
        }
        running += d;
        cmnd[lag] = running > 0 ? d * lag / running : 1.0;
    }
    int best = -1;
    for (int lag = minLag; lag <= maxLag; lag++) {
        if (cmnd[lag] < 0.25 && cmnd[lag] <= cmnd[lag + 1]) { best = lag; break; }
    }
    if (best < 0) {
        best = minLag;
        for (int lag = minLag; lag <= maxLag; lag++) if (cmnd[lag] < cmnd[best]) best = lag;
    }
    aperiodicity = cmnd[best];
    const double a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
    const double shift = (a - 2 * b + c) != 0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0;
    return 48000.0 / (best + shift);
}

// Energy-weighted mean pitch (semitones re A4) of the spectrum between 60 Hz and 6 kHz, over 8192 samples at `startSec`.
double centroid(const std::vector<double>& x, double startSec) {
    const size_t start = size_t(startSec * 48000), n = 8192;
    if (x.size() < start + n) return 0.0;
    double num = 0, den = 0;
    for (size_t k = 11; k < n / 2; k++) {  // Plain DFT on bins 60 Hz .. 6 kHz, every 2nd bin
        const double f = 48000.0 * double(k) / double(n);
        if (f > 6000) break;
        if (k % 2) continue;
        double re = 0, im = 0;
        for (size_t i = 0; i < n; i++) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(n - 1));
            const double a = 2 * kPi * double(k) * double(i) / double(n);
            re += x[start + i] * w * std::cos(a);
            im -= x[start + i] * w * std::sin(a);
        }
        const double power = re * re + im * im;
        num += power * 12 * std::log2(f / 440.0);
        den += power;
    }
    return den > 0 ? num / den + 69 : 0;
}

// Milliseconds until the 10 ms RMS envelope first reaches half its maximum (within the rendering).
double attackMs(const std::vector<double>& x) {
    const size_t hop = 480;
    std::vector<double> env;
    for (size_t i = 0; i + hop <= x.size(); i += hop) {
        double s = 0;
        for (size_t j = 0; j < hop; j++) s += x[i + j] * x[i + j];
        env.push_back(std::sqrt(s / hop));
    }
    double peak = 0;
    for (double v : env) peak = std::max(peak, v);
    for (size_t i = 0; i < env.size(); i++) {
        if (env[i] >= 0.5 * peak) return 10.0 * double(i);
    }
    return 0;
}

double rms(const std::vector<double>& x) {
    double s = 0; for (double v : x) s += v * v; return x.empty() ? 0 : std::sqrt(s / x.size());
}

}  // namespace

int main(int argc, char** argv) {
    EngineOptions options;
    options.midiTimestamping = false;
    std::string error;
    SynthEngine mt, dx;
    mt.setOptions(options);
    dx.setOptions(options);
    const std::vector<RomEntry> roms = scanRomFolder(findRomFolder({Platform::executableDirectory(), std::filesystem::current_path()}));
    EngineConfig mc, dc;
    mc.outputSampleRate = dc.outputSampleRate = 48000;
    for (const RomEntry& rom : roms) {
        EngineConfig& config = romFamily(rom) == "d110" ? dc : romFamily(rom) == "mt32" ? mc : mc;
        if (romFamily(rom) != "d110" && romFamily(rom) != "mt32") continue;
        (rom.isControl ? config.controlRom : config.pcmRom) = rom.path;
    }
    if (mc.controlRom.empty() || mc.pcmRom.empty() || dc.controlRom.empty() || dc.pcmRom.empty()) {
        std::printf("Needs MT-32 and D-110 control and PCM ROMs in the ROM folder\n");
        return 2;
    }
    if (!mt.configure(mc, error) || !dx.configure(dc, error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    const std::vector<std::string> dxNames = dx.toneNames();
    const std::vector<std::string> mtNames = mt.toneNames();
    std::vector<int> only;
    std::vector<std::array<int, 3>> pairs;
    for (int i = 1; i < argc; i++) {
        int a, b, c;
        if (std::sscanf(argv[i], "%d:%d:%d", &a, &b, &c) == 3) pairs.push_back({a, b, c});
        else only.push_back(std::atoi(argv[i]));
    }
    double lastCentroid = 0;
    auto measureD110 = [&](int tone, int shift, double& ap, double& level) {
        const uint8_t temp[3] = {uint8_t(tone / 64), uint8_t(tone % 64), uint8_t(24 + shift)};
        dx.writePartTemp(0, 0, temp, 3);
        std::vector<double> x = renderNote(dx, 0, 0.8);
        level = rms(x);
        lastCentroid = centroid(x, 0.25);
        return yin(x, 0.25, ap);
    };
    for (const std::array<int, 3>& pair : pairs) {
        mt.onMidiShortMessage(0xC1 | (uint32_t(pair[0]) << 8));
        std::vector<double> x = renderNote(mt, 1, 1.5);
        double apM = 1, apD = 1, levelD = 0;
        const double fm = yin(x, 0.4, apM), cm = centroid(x, 0.4), am = attackMs(x);
        const uint8_t temp[3] = {uint8_t(pair[1] / 64), uint8_t(pair[1] % 64), uint8_t(24 + pair[2])};
        dx.writePartTemp(0, 0, temp, 3);
        std::vector<double> y = renderNote(dx, 0, 1.5);
        const double fd = yin(y, 0.4, apD), cd = centroid(y, 0.4), ad = attackMs(y);
        std::printf("%c%-2d %-10s %7.1f Hz attack %4.0f ms | %c%d%d %-10s %+3d %7.1f Hz attack %4.0f ms | %+6.1f st centroid %+6.1f\n",
                    pair[0] < 64 ? 'A' : 'B', pair[0] % 64 + 1, mtNames[size_t(pair[0])].c_str(), fm, am, "ab"[pair[1] / 64], pair[1] % 64 / 8 + 1,
                    pair[1] % 8 + 1, dxNames[size_t(pair[1])].c_str(), pair[2], fd, ad, 12 * std::log2(fm / fd), cm - cd);
        (void)levelD;
    }
    if (!pairs.empty()) return 0;
    for (int p = 0; p < 128; p++) {
        if (!only.empty() && std::find(only.begin(), only.end(), p) == only.end()) continue;
        mt.onMidiShortMessage(0xC1 | (uint32_t(p) << 8));
        std::vector<double> x = renderNote(mt, 1, 0.8);
        double apM = 1, apD = 1, levelD = 0;
        const double fm = yin(x, 0.25, apM);
        const double cm = centroid(x, 0.25);
        const int tone = Mt32Translator::presetTone(p), shift = Mt32Translator::presetKeyShift(p);
        if (tone >= 128) continue;
        const double fd = measureD110(tone, shift, apD, levelD);
        const double diff = (fm > 0 && fd > 0) ? 12 * std::log2(fm / fd) : 0;
        std::printf("%c%-2d %-10s %7.1f Hz (%.2f) | %c%d%d %-10s %+3d %7.1f Hz (%.2f) | %+6.1f st  centroid %+6.1f%s\n", p < 64 ? 'A' : 'B', p % 64 + 1,
                    mtNames[size_t(p)].c_str(), fm, apM, "ab"[tone / 64], tone % 64 / 8 + 1, tone % 8 + 1, dxNames[size_t(tone)].c_str(), shift, fd, apD,
                    diff, cm - lastCentroid, std::fabs(diff) > 6 && apM < 0.3 && apD < 0.3 ? "  <==" : "");
    }
}
