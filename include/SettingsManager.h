#pragma once
#include <string>
#include <vector>
#include <map>

/**
 * @brief Manages saving and loading of application state (JSON).
 */
class SettingsManager {
public:
    SettingsManager() = default;
    ~SettingsManager() = default;

    /**
     * @brief Load settings from a JSON file.
     * @param filepath Path to the JSON file.
     * @return true if successful.
     */
    bool load(const std::string& filepath);

    /**
     * @brief Save settings to a JSON file.
     * @param filepath Path to the JSON file.
     * @return true if successful.
     */
    bool save(const std::string& filepath) const;

    // --- State Accessors ---
    
    // Engine State
    double bpm = 120.0;
    int subdivision = 1;
    int timeSigTop = 4;
    int timeSigBottom = 4;
    bool isMuted = false;
    double grooveShiftMs = 0.0;
    
    // Instrument / Tuner State
    int tunerMode = 0; // Maps to PreciseTuner::Mode
    
    // Plugin States
    std::map<std::string, bool> pluginStates;
};
