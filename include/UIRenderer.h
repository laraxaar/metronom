#pragma once
#include "FontRenderer.h"
#include "TempoCoach.h"
#include "RhythmGrid.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

struct GLFWwindow;

struct UIState {
    // Metronome
    double bpm = 120.0;
    int timeSigTop = 4;
    int timeSigBottom = 4;
    bool playing = false;
    bool freePlay = false;
    float groove = 0.0f;
    int subdivision = 4;  // 4,8,12,16,20

    // Grid (from GridSnapshot)
    int gridNumSteps = 4;
    int gridCurrentStep = -1;  // playhead (-1 = stopped)
    uint8_t gridStates[32]{};  // StepState values

    // Analysis
    float peakInput = 0.0f;
    float peakOutput = 0.0f;
    float cpuLoad = 0.0f;
    float accuracy = 0.0f;
    std::string scoreRank = "-";
    GrooveProfile grooveProfile;
    // Free Play / Live tempo
    double playerBpm = 0.0;
    float playerStability = 0.0f; // 0..100
    std::string coachText;
    bool flowActive = false;

    // Tuner
    float currentFreq = 0.0f;
    float diffCents = 0.0f;
    float tunerConfidence = 0.0f;
    float tunerSignalLevel = 0.0f;
    std::string noteName = "-";
    int tunerMode = 0;  // 0=Chromatic, 1=GuitarStd, 2=DropD, 3=DropC, 4=BassStd, 5=Bass5Str, 6=DropA

    // Mixer
    float masterVol = 0.8f;
    float clickVol = 0.7f;
    float accentVol = 0.95f;
    float inputVol = 0.5f;
    float masterPeak = 0.0f;
    float clickPeak = 0.0f;
    float inputPeak = 0.0f;

    // Training
    bool ladderEnabled = false;
    int ladderBars = 4;
    float ladderInc = 5.0f;
    bool silenceEnabled = false;
    float silenceProb = 0.2f;
    bool disappearingEnabled = false;
    int disappearVisible = 4;
    int disappearHidden = 4;
    bool grooveEnabled = false;
    float grooveMaxShiftMs = 0.0f;
    bool drunkenEnabled = false;
    float drunkenLevel = 0.0f;
    bool bossEnabled = false;
    float bossLevel = 0.5f;
    float bossFlash = 0.0f;
    bool bossGameOver = false;
    bool humanTestEnabled = false;

    // Polyrhythm trainer
    bool polyEnabled = false;
    int polyX = 0;
    int polyY = 0;

    // Coach history (weekly)
    char histDate[7][11]{};
    float histAcc[7]{};
    float histStab[7]{};
    float histMinutes[7]{};

    // Mouse
    double mouseX = 0, mouseY = 0;
    bool mouseDown = false;
    bool mousePressed = false;
    int windowWidth = 1280;
    int windowHeight = 720;
};

enum class UIEventType {
    None,
    TogglePlay,
    BpmChange,
    GrooveChange,
    TimeSigChange,
    SubdivisionChange,
    GridStepCycle,
    TapTempo,
    SwitchTab,
    TunerModeChange,
    MixerMasterChange,
    MixerClickChange,
    MixerAccentChange,
    MixerInputChange,
    SetInstrument,
    SetTuning,
    TrainingToggle,
    TrainingParamAdjust,
    HumanTestToggle,
    PolyrhythmSet,
    PolyrhythmClear,
    FreePlayToggle,
    ToggleTrainingDrawer,
};

struct UIEvent {
    UIEventType type = UIEventType::None;
    float value = 0.0f;
    int intValue = 0;
};

class UIRenderer {
public:
    UIRenderer(GLFWwindow* window);
    ~UIRenderer();

    void newFrame();
    UIEvent render(const UIState& state);

private:
    // --- Geometry (GL 3.3 Core) ---
    void initShaders();
    void drawRect(float x, float y, float w, float h, float r, float g, float b, float a = 1.0f);
    void drawRoundedRect(float x, float y, float w, float h, float radius, float r, float g, float b, float a = 1.0f);
    void drawCircle(float cx, float cy, float radius, float r, float g, float b, float a = 1.0f);

    // --- UI Helpers ---
    bool drawButton(float x, float y, float w, float h, const std::string& text, bool active, const UIState& state, float fontSize = 0.35f);
    bool drawSmallButton(float x, float y, float w, float h, const std::string& text, bool active, const UIState& state);
    void drawFader(float x, float y, float w, float h, float value, float peak, const std::string& label, const UIState& state, float r, float g, float b);
    void drawPeakMeter(float x, float y, float w, float h, float level);
    void drawText(float x, float y, const std::string& text, float size = 0.4f, float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);
    void drawTextCentered(float cx, float y, const std::string& text, float size, float r, float g, float b, float a = 1.0f);

    // --- Sections ---
    UIEvent renderHeader(const UIState& state, float x, float y, float w);
    UIEvent renderMatrix(const UIState& state, float x, float y, float w, float h);
    UIEvent renderTuner(const UIState& state, float x, float y, float w, float h);
    UIEvent renderMixer(const UIState& state, float x, float y, float w, float h);
    void renderCoachTab(const UIState& state, float x, float y, float w, float h);
    UIEvent renderTrainingDrawer(const UIState& state, float W, float H);

    bool isHovered(float x, float y, float w, float h, const UIState& state) const;

    GLFWwindow* m_window;
    std::unique_ptr<FontRenderer> m_fontRenderer;
    int m_activeTab = 0;
    bool m_trainingDrawerOpen = false;
    float m_trainingDrawerAnim = 0.0f; // 0..1

    // GL 3.3 geometry shader
    unsigned int m_geoShader = 0;
    unsigned int m_geoVAO = 0, m_geoVBO = 0;
};