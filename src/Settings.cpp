#include "Settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace

bool Settings::load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    loadText(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    return true;
}

bool Settings::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << text();
    return bool(out);
}

void Settings::loadText(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string content = trim(line);
        if (content.empty() || content[0] == '#' || content[0] == ';') continue;
        const size_t separator = content.find('=');
        if (separator == std::string::npos) continue;
        values_[trim(content.substr(0, separator))] = trim(content.substr(separator + 1));
    }
}

std::string Settings::text() const {
    std::string text = "# D110Emu settings\n";
    for (const auto& entry : values_) text += entry.first + " = " + entry.second + "\n";
    return text;
}

std::string Settings::getString(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Settings::getInt(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    return *end == '\0' ? int(value) : fallback;
}

float Settings::getFloat(const std::string& key, float fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    return *end == '\0' ? value : fallback;
}

bool Settings::getBool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (it->second == "1" || it->second == "true") return true;
    if (it->second == "0" || it->second == "false") return false;
    return fallback;
}

void Settings::set(const std::string& key, const std::string& value) {
    values_[key] = value;
}

void Settings::set(const std::string& key, int value) {
    values_[key] = std::to_string(value);
}

void Settings::set(const std::string& key, float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", double(value));
    values_[key] = text;
}

void Settings::set(const std::string& key, bool value) {
    values_[key] = value ? "true" : "false";
}
