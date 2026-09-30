#include "ReverbSettingsFile.h"

#include <cstdio>
#include <cstdlib>

#include "Platform.h"
#include "Settings.h"

namespace {

using MT32Emu::DSeriesReverbSettings;
using MT32Emu::DSeriesReverbType;

const char* const kTypeNames[8] = {"Small Room", "Medium Room", "Medium Hall", "Large Hall", "Plate", "Delay 1", "Delay 2", "Delay 3"};

std::string prefix(int type) {
    return "type" + std::to_string(type + 1) + ".";
}

// Reads a comma-separated list into `values`; entries the file lacks stay as they are.
void readList(const Settings& file, const std::string& key, float* values, int count) {
    const std::string text = file.getString(key);
    const char* p = text.c_str();
    for (int i = 0; i < count && *p != 0; i++) {
        char* end = nullptr;
        const double value = std::strtod(p, &end);
        if (end == p) break;
        values[i] = float(value);
        p = end;
        while (*p == ' ' || *p == ',') p++;
    }
}

std::string list(const float* values, int count) {
    std::string text;
    for (int i = 0; i < count; i++) {
        char number[32];
        std::snprintf(number, sizeof(number), "%g", double(values[i]));
        text += (i > 0 ? "," : "") + std::string(number);
    }
    return text;
}

}  // namespace

bool loadReverbSettings(const std::filesystem::path& path, DSeriesReverbSettings& settings, std::string& error) {
    Settings file;
    if (!file.load(path)) {
        error = "Cannot read " + Platform::toUtf8(path);
        return false;
    }
    settings = DSeriesReverbSettings::getDefaults();
    for (int t = 0; t < 8; t++) {
        DSeriesReverbType& type = settings.types[t];
        const std::string p = prefix(t);
        type.sendDb = file.getFloat(p + "send_db", type.sendDb);
        type.bandwidthHz = file.getFloat(p + "bandwidth_hz", type.bandwidthHz);
        type.dampingHz = file.getFloat(p + "damping_hz", type.dampingHz);
        type.width = file.getFloat(p + "width", type.width);
        readList(file, p + "wet_db", type.wetDb, 8);
        if (!type.delay) {
            type.preDelayMs = file.getFloat(p + "pre_delay_ms", type.preDelayMs);
            readList(file, p + "allpass_ms", type.allpassMs, int(DSeriesReverbType::ALLPASS_COUNT));
            type.diffusion = file.getFloat(p + "diffusion", type.diffusion);
            readList(file, p + "comb_ms", type.combMs, int(DSeriesReverbType::COMB_COUNT));
            type.size = file.getFloat(p + "size", type.size);
            readList(file, p + "rt60", type.rt60, 8);
        } else {
            readList(file, p + "delay_l_ms", type.delayLMs, 8);
            readList(file, p + "delay_r_ms", type.delayRMs, 8);
            type.feedback = file.getFloat(p + "feedback", type.feedback);
        }
    }
    settings.clamp();
    return true;
}

bool saveReverbSettings(const std::filesystem::path& path, const DSeriesReverbSettings& settings, std::string& error) {
    Settings file;
    for (int t = 0; t < 8; t++) {
        const DSeriesReverbType& type = settings.types[t];
        const std::string p = prefix(t);
        file.set(p + "name", kTypeNames[t]);  // For the reader: not read back
        file.set(p + "send_db", type.sendDb);
        file.set(p + "bandwidth_hz", type.bandwidthHz);
        file.set(p + "damping_hz", type.dampingHz);
        file.set(p + "width", type.width);
        file.set(p + "wet_db", list(type.wetDb, 8));
        if (!type.delay) {
            file.set(p + "pre_delay_ms", type.preDelayMs);
            file.set(p + "allpass_ms", list(type.allpassMs, int(DSeriesReverbType::ALLPASS_COUNT)));
            file.set(p + "diffusion", type.diffusion);
            file.set(p + "comb_ms", list(type.combMs, int(DSeriesReverbType::COMB_COUNT)));
            file.set(p + "size", type.size);
            file.set(p + "rt60", list(type.rt60, 8));
        } else {
            file.set(p + "delay_l_ms", list(type.delayLMs, 8));
            file.set(p + "delay_r_ms", list(type.delayRMs, 8));
            file.set(p + "feedback", type.feedback);
        }
    }
    if (!file.save(path)) {
        error = "Cannot write " + Platform::toUtf8(path);
        return false;
    }
    return true;
}
