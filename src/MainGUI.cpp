/**
 * @file MainGUI.cpp
 * @brief High-performance UI for the Metronome Engine using Dear ImGui.
 * Connects to the AudioEngine via UIController.
 */

#include "imgui.h"
#include "UIController.h"
#include <vector>
#include <string>

// Helper to render the oscilloscope
void RenderOscilloscope(const std::vector<float>& waveform) {
    if (waveform.empty()) return;
    
    ImGui::Text("Input Waveform (Real-time)");
    ImGui::PlotLines("##Oscilloscope", waveform.data(), static_cast<int>(waveform.size()), 
                     0, nullptr, -1.0f, 1.0f, ImVec2(0, 150));
}

// Helper to render the Tuner
void RenderTuner(float frequency, float confidence) {
    ImGui::BeginChild("Tuner", ImVec2(0, 100), true);
    ImGui::Text("Precise Tuner");
    
    if (confidence > 0.4f) {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Frequency: %.2f Hz", frequency);
        ImGui::ProgressBar(confidence, ImVec2(0, 0), "Confidence");
    } else {
        ImGui::TextColored(ImVec4(0.5, 0.5, 0.5, 1), "Detecting...");
    }
    
    ImGui::EndChild();
}

// Main GUI Loop (called every frame from the main thread)
void RenderMainGUI(UIController& controller) {
    auto visualData = controller.getLatestVisualData();
    
    ImGui::SetNextWindowSize(ImVec2(500, 600), ImGuiCond_FirstUseEver);
    ImGui::Begin("Advanced Metronome Engine");

    // --- Section 1: Metronome Controls ---
    ImGui::Text("Core Metronome");
    static float bpm = 120.0f;
    if (ImGui::SliderFloat("BPM", &bpm, 10.0f, 1000.0f)) {
        controller.setBpm(static_cast<double>(bpm));
    }

    static int subdivision = 1;
    const char* subdivisions[] = { "1/4", "1/8", "1/16", "1/32" };
    ImGui::Combo("Subdivision", &subdivision, subdivisions, IM_ARRAYSIZE(subdivisions));

    static bool muted = false;
    if (ImGui::Checkbox("Mute Metronome", &muted)) {
        controller.setMuted(muted);
    }

    ImGui::Separator();

    // --- Section 2: Visualizers ---
    if (visualData) {
        RenderOscilloscope(visualData->waveform);
        RenderTuner(visualData->currentFrequency, visualData->confidence);
    }

    ImGui::Separator();

    // --- Section 3: Training Plugins ---
    if (ImGui::CollapsingHeader("Training Modules")) {
        static bool ladderEnabled = false;
        ImGui::Checkbox("BPM Ladder", &ladderEnabled);
        
        static bool randomSilence = false;
        ImGui::Checkbox("Random Silence", &randomSilence);
        
        static bool humanTest = false;
        ImGui::Checkbox("Human Metronome Test", &humanTest);
    }

    ImGui::End();
}
