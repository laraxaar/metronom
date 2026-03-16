#include "MainGUI.h"
#include "imgui.h"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <cstdio>

MainGUI::MainGUI(std::shared_ptr<UIController> uiCtrl, AudioEngine* engine, SettingsManager* settings)
    : m_uiCtrl(uiCtrl), m_engine(engine), m_settings(settings) {
    
    // Load initial state from settings
    m_bpm = std::max(10, static_cast<int>(m_settings->bpm));
    m_timeSigTop = m_settings->timeSigTop;
    m_timeSigBottom = m_settings->timeSigBottom;
    
    // Initialize default pattern (First beat accented, others normal)
    m_beatPattern.resize(m_timeSigTop, 1);
    if (!m_beatPattern.empty()) m_beatPattern[0] = 2; // Accent
    updateEngineBeatPattern();
}

void MainGUI::updateEngineBeatPattern() {
    if (m_engine) {
        m_engine->setBeatPattern(m_beatPattern);
    }
}

void MainGUI::render() {
    VisualData data = m_uiCtrl->getLatestVisualData();

    // Setup Main Window
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1024, 768), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Advanced Metronome", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar)) {
        drawTopBar(data);
        
        ImGui::Columns(2, "MainColumns");
        ImGui::SetColumnWidth(0, 450); // Left panel width
        
        // Left Column: Controls
        drawTransportAndBeatControls(data);
        
        ImGui::NextColumn();
        
        // Right Column: Visualizers
        drawTunerAndScope(data);
        drawTempoCoach(data);
        
        ImGui::Columns(1); // Reset columns
    }
    ImGui::End();
}

void MainGUI::drawTopBar(const VisualData& data) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Settings")) {
                m_settings->bpm = m_bpm;
                m_settings->timeSigTop = m_timeSigTop;
                m_settings->timeSigBottom = m_timeSigBottom;
                m_settings->save("settings.json");
            }
            if (ImGui::MenuItem("Load Settings")) {
                if (m_settings->load("settings.json")) {
                    m_bpm = static_cast<int>(m_settings->bpm);
                    m_timeSigTop = m_settings->timeSigTop;
                    m_timeSigBottom = m_settings->timeSigBottom;
                    m_beatPattern.resize(m_timeSigTop, 1);
                    if (!m_beatPattern.empty()) m_beatPattern[0] = 2;
                    m_engine->setBpm(m_bpm);
                    updateEngineBeatPattern();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    
    // Header Stats
    ImGui::Text("CPU Load: %.1f%%", data.cpuLoad);
    ImGui::SameLine(ImGui::GetWindowWidth() - 250);
    ImGui::Text("Peaks: In %.2f | Out %.2f", data.peakInput, data.peakOutput);
    ImGui::Separator();
}

void MainGUI::drawTransportAndBeatControls(const VisualData& data) {
    ImGui::Text("TRANSPORT & TEMPO");
    ImGui::Spacing();
    
    // BPM Control
    ImGui::PushItemWidth(300);
    if (ImGui::SliderInt("BPM", &m_bpm, 10, 400)) {
        m_engine->setBpm(m_bpm);
    }
    ImGui::PopItemWidth();
    
    if (ImGui::Button("-1", ImVec2(40, 30))) { m_bpm--; m_engine->setBpm(m_bpm); }
    ImGui::SameLine();
    if (ImGui::Button("+1", ImVec2(40, 30))) { m_bpm++; m_engine->setBpm(m_bpm); }
    ImGui::SameLine();
    if (ImGui::Button("TAP TEMPO", ImVec2(208, 30))) {
        // TBD: Hook to TapDetector
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::Text("TIME SIGNATURE & PATTERN");
    
    // Time Signature
    bool sigChanged = false;
    ImGui::PushItemWidth(100);
    if (ImGui::InputInt("Beats", &m_timeSigTop)) sigChanged = true;
    ImGui::SameLine();
    ImGui::Text("/");
    ImGui::SameLine();
    if (ImGui::InputInt("Value", &m_timeSigBottom)) sigChanged = true;
    ImGui::PopItemWidth();
    
    if (m_timeSigTop < 1) m_timeSigTop = 1;
    if (m_timeSigTop > 32) m_timeSigTop = 32;
    // Keep bottom as power of 2
    if (m_timeSigBottom < 1) m_timeSigBottom = 1;
    
    if (sigChanged) {
        m_beatPattern.resize(m_timeSigTop, 1);
        updateEngineBeatPattern();
    }
    
    ImGui::Spacing();
    
    // Per-Beat Controls
    ImGui::Text("Beat Pattern (Click to toggle: Normal -> Accent -> Mute)");
    for (int i = 0; i < m_timeSigTop; ++i) {
        ImGui::PushID(i);
        
        // Color based on state
        ImVec4 col;
        if (m_beatPattern[i] == 2) col = ImVec4(0.9f, 0.2f, 0.2f, 1.0f); // Accent (Red)
        else if (m_beatPattern[i] == 1) col = ImVec4(0.2f, 0.7f, 0.2f, 1.0f); // Normal (Green)
        else col = ImVec4(0.3f, 0.3f, 0.3f, 1.0f); // Mute (Grey)
        
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x*1.2f, col.y*1.2f, col.z*1.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x*0.8f, col.y*0.8f, col.z*0.8f, 1.0f));
        
        char label[16];
        std::snprintf(label, sizeof(label), "%d", i + 1);
        if (ImGui::Button(label, ImVec2(40, 40))) {
            // Cycle: 1 (Normal) -> 2 (Accent) -> 0 (Mute) -> 1
            if (m_beatPattern[i] == 1) m_beatPattern[i] = 2;
            else if (m_beatPattern[i] == 2) m_beatPattern[i] = 0;
            else m_beatPattern[i] = 1;
            
            updateEngineBeatPattern();
        }
        
        ImGui::PopStyleColor(3);
        
        // Wrap buttons nicely (8 per row)
        if ((i + 1) % 8 != 0 && i != m_timeSigTop - 1) {
            ImGui::SameLine();
        }
        ImGui::PopID();
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    
    // Instrument Optimization Mode
    ImGui::Spacing();
    ImGui::Text("INSTRUMENT MODE");
    const char* modes[] = { "Guitar Standard", "Guitar Drop D", "Bass Standard", "Bass 5-String", "Custom / Drums" };
    static int currentMode = 0;
    if (ImGui::Combo("Optimization", &currentMode, modes, IM_ARRAYSIZE(modes))) {
        PreciseTuner::Mode m = static_cast<PreciseTuner::Mode>(currentMode);
        m_engine->setInstrumentMode(m);
        m_settings->tunerMode = currentMode;
    }
}

void MainGUI::drawTunerAndScope(const VisualData& data) {
    ImGui::Text("PRECISE TUNER");
    
    // Tuner Display
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
    char tunerText[64];
    if (data.confidence > 0.4f) {
        std::snprintf(tunerText, sizeof(tunerText), "%.1f Hz", data.currentFrequency);
    } else {
        std::snprintf(tunerText, sizeof(tunerText), "Tuning...");
    }
    
    // Confidence Bar
    ImGui::ProgressBar(data.confidence, ImVec2(-1, 25), tunerText);
    ImGui::PopStyleColor();
    
    ImGui::Spacing();
    
    // Oscilloscope
    ImGui::Text("OSCILLOSCOPE");
    if (!data.waveform.empty()) {
        ImGui::PlotLines("##Oscilloscope", data.waveform.data(), static_cast<int>(data.waveform.size()), 0, nullptr, -1.0f, 1.0f, ImVec2(-1, 120));
    } else {
        ImGui::Dummy(ImVec2(-1, 120));
    }
    ImGui::Separator();
}

void MainGUI::drawTempoCoach(const VisualData& data) {
    ImGui::Spacing();
    ImGui::Text("TEMPO COACH (ANALYTICS)");
    
    if (data.hitDeviations.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Waiting for hits...");
        return;
    }
    
    // Draw scatter plot for deviations
    // Deviation is in ms, typical range -100ms (early) to +100ms (late)
    ImGui::PlotHistogram("Deviation (ms)", data.hitDeviations.data(), static_cast<int>(data.hitDeviations.size()), 0, nullptr, -100.0f, 100.0f, ImVec2(-1, 150));
    
    // Calculate average
    float sum = 0.0f;
    for (float v : data.hitDeviations) sum += std::abs(v);
    float avg = sum / static_cast<float>(std::max((size_t)1, data.hitDeviations.size()));
    
    ImGui::Text("Average Error: %.1f ms", avg);
    
    if (avg < 15.0f) ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Grade: EXCELLENT");
    else if (avg < 30.0f) ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Grade: GOOD");
    else ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Grade: NEEDS WORK");
}
