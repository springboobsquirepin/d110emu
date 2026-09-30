#pragma once

#include <filesystem>
#include <map>
#include <string>

// Flat "key = value" settings file (UTF-8). Unknown keys are preserved on save.
class Settings {
public:
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;
    // The file's contents, for keeping the settings elsewhere (a plugin host's project). loadText() adds to what is there.
    void loadText(const std::string& text);
    std::string text() const;

    std::string getString(const std::string& key, const std::string& fallback = std::string()) const;
    int getInt(const std::string& key, int fallback) const;
    float getFloat(const std::string& key, float fallback) const;
    bool getBool(const std::string& key, bool fallback) const;

    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, const char* value) { set(key, std::string(value)); }
    void set(const std::string& key, int value);
    void set(const std::string& key, float value);
    void set(const std::string& key, bool value);

private:
    std::map<std::string, std::string> values_;
};
