#include "SettingsManager.h"
#include <fstream>
#include <iostream>

// Since we don't have nlohmann/json in the include dir yet, we'll write a simple 
// fallback parser/serializer or placeholder until the dependency is confirmed.
// For now, we will implement a basic key-value text format to ensure it works without external deps.
// In a real production scenario with CMake, we'd use nlohmann::json.

bool SettingsManager::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        size_t delim = line.find('=');
        if (delim == std::string::npos) continue;

        std::string key = line.substr(0, delim);
        std::string val = line.substr(delim + 1);

        try {
            if (key == "bpm") bpm = std::stod(val);
            else if (key == "subdivision") subdivision = std::stoi(val);
            else if (key == "timeSigTop") timeSigTop = std::stoi(val);
            else if (key == "timeSigBottom") timeSigBottom = std::stoi(val);
            else if (key == "isMuted") isMuted = (val == "1" || val == "true");
            else if (key == "tunerMode") tunerMode = std::stoi(val);
            else if (key == "grooveShiftMs") grooveShiftMs = std::stod(val);
            else if (key.find("plugin_") == 0) {
                std::string pluginName = key.substr(7);
                pluginStates[pluginName] = (val == "1" || val == "true");
            }
        } catch (...) {
            std::cerr << "SettingsManager: Failed to parse value for key " << key << std::endl;
        }
    }
    return true;
}

bool SettingsManager::save(const std::string& filepath) const {
    std::ofstream file(filepath);
    if (!file.is_open()) return false;

    file << "bpm=" << bpm << "\n";
    file << "subdivision=" << subdivision << "\n";
    file << "timeSigTop=" << timeSigTop << "\n";
    file << "timeSigBottom=" << timeSigBottom << "\n";
    file << "isMuted=" << (isMuted ? "1" : "0") << "\n";
    file << "tunerMode=" << tunerMode << "\n";
    file << "grooveShiftMs=" << grooveShiftMs << "\n";

    for (const auto& kv : pluginStates) {
        file << "plugin_" << kv.first << "=" << (kv.second ? "1" : "0") << "\n";
    }

    return true;
}
