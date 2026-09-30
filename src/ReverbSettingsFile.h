#pragma once

#include <filesystem>
#include <string>

#include "DSeriesReverb.h"

// d110emu-reverb.ini: the D-series reverb's parameters (MT32Emu::DSeriesReverbSettings) as "typeN.key = value" lines,
// N = 1-8 (Small Room to Delay 3), lists separated by commas. Keys a file lacks keep the defaults, so a file may hold
// one type or one value only. d110emu keeps its tuning there; tools/reverb_analysis.py writes fitted values in the same
// form.
bool loadReverbSettings(const std::filesystem::path& path, MT32Emu::DSeriesReverbSettings& settings, std::string& error);
bool saveReverbSettings(const std::filesystem::path& path, const MT32Emu::DSeriesReverbSettings& settings, std::string& error);
