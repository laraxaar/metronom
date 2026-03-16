#pragma once
#include "UIController.h"
#include "AudioEngine.h"
#include "SettingsManager.h"
#include <memory>
#include <vector>

/**
 * @brief Main Graphical User Interface using Dear ImGui.
 */
class MainGUI {
public:
    MainGUI(std::shared_ptr<UIController> uiCtrl, AudioEngine* engine, SettingsManager* settings);
    ~MainGUI() = default;

    /**
     * @brief Render the ImGui application frame.
     * Call this inside your main rendering loop (e.g., GLFW/OpenGL).
     */
    void render();

private:
    std::shared_ptr<UIController> m_uiCtrl;
    AudioEngine* m_engine;
    SettingsManager* m_settings;

    // Local UI State
    int m_bpm = 120;
    int m_timeSigTop = 4;
    int m_timeSigBottom = 4;
    
    // Beat Pattern (0 = Mute, 1 = Normal, 2 = Accent)
    std::vector<int> m_beatPattern;

    // Helper rendering methods
    void drawTopBar(const VisualData& data);
    void drawTunerAndScope(const VisualData& data);
    void drawTransportAndBeatControls(const VisualData& data);
    void drawTempoCoach(const VisualData& data);
    
    void updateEngineBeatPattern();
};
