#pragma once

// For the tools that run the plugins' code (vsttest, vst3test, vst3host): the plugins keep their computer-wide settings
// in the user's D110Emu folder (Platform::userDataDirectory), so the tools point D110EMU_DATA_HOME at a folder of their
// own in the temporary folder and leave the real one alone.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// The folder useTestDataFolder chose (empty: whoever runs the tool chose one).
inline std::filesystem::path& testDataFolder() {
    static std::filesystem::path folder;
    return folder;
}

// `fresh`: emptied first, for tests that must start from nothing; else kept between runs, and one that whoever runs the
// tool chose (D110EMU_DATA_HOME already set) is left as it is.
inline void useTestDataFolder(const char* name, bool fresh) {
    if (!fresh && std::getenv("D110EMU_DATA_HOME") != nullptr) return;
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    if (fresh) std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    testDataFolder() = folder;
#ifdef _WIN32
    _wputenv_s(L"D110EMU_DATA_HOME", folder.wstring().c_str());
#else
    setenv("D110EMU_DATA_HOME", folder.c_str(), 1);
#endif
}

// Sets a key of the plugins' computer-wide settings (d110emu-vst.ini in the test's D110Emu folder), as the plugins'
// configuration window does: new instances read them (the layout of the MULTI outputs, say).
inline void setTestPluginSetting(const std::string& key, const std::string& value) {
    if (testDataFolder().empty()) return;
    const std::filesystem::path folder = testDataFolder() / "D110Emu";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / "d110emu-vst.ini";
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            const bool same = line.compare(0, key.size(), key) == 0 && (line.size() == key.size() || line[key.size()] == ' ' || line[key.size()] == '=');
            if (!same) lines.push_back(line);
        }
    }
    lines.push_back(key + " = " + value);
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& line : lines) out << line << '\n';
}
