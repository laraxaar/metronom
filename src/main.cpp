#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "AudioEngine.h"
#include "UIController.h"
#include "SettingsManager.h"
#include "UIRenderer.h"

#include <iostream>
#include <memory>
#include <chrono>
#include <cstring>

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "Glfw Error " << error << ": " << description << std::endl;
}

int main(int, char**) {
    // 1. Setup GLFW
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "M E T R O N O M", NULL, NULL);
    if (window == NULL) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    // 2. Initialize Audio Engine
    auto settings = std::make_unique<SettingsManager>();
    settings->load("settings.json");

    auto uiController = std::make_shared<UIController>();
    auto audioEngine = std::make_unique<AudioEngine>();

    audioEngine->setUIController(uiController);
    if (!audioEngine->initialize("config_schema.json")) {
        std::cerr << "Failed to initialize Audio Engine!" << std::endl;
        return 1;
    }
    audioEngine->start();

    // 3. Initialize UI
    UIRenderer uiRenderer(window);
    UIState uiState;

    // 4. Main Loop
    bool lastMouseDown = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Window size
        glfwGetFramebufferSize(window, &uiState.windowWidth, &uiState.windowHeight);

        // --- Populate UIState from AudioEngine ---
        uiState.playing = audioEngine->isClickEnabled();
        uiState.freePlay = audioEngine->isFreePlayActive();
        uiState.bpm = audioEngine->getLiveBpm();
        uiState.cpuLoad = audioEngine->getCpuLoad();
        uiState.accuracy = audioEngine->getAccuracy();
        uiState.scoreRank = audioEngine->getScoreRank();
        uiState.grooveProfile = audioEngine->getGrooveProfile();
        uiState.playerBpm = audioEngine->getLivePlayerBpm();
        uiState.playerStability = audioEngine->getLivePlayerStability();

        // Live coach text (seqlock read)
        {
            std::string msg;
            for (int tries = 0; tries < 3; ++tries) {
                uint32_t s1 = audioEngine->getLiveCoachSeq();
                if (s1 & 1u) continue;
                const char* p = audioEngine->getLiveCoachText();
                msg = p ? p : "";
                uint32_t s2 = audioEngine->getLiveCoachSeq();
                if (s1 == s2 && ((s2 & 1u) == 0)) break;
            }
            uiState.coachText = msg;
            uiState.flowActive = audioEngine->isFlowActive();
        }

        // Tuner data from TunerResult
        const auto& tunerResult = audioEngine->getTunerResult();
        uiState.currentFreq = tunerResult.currentFreq.load();
        uiState.diffCents = tunerResult.diffCents.load();
        uiState.tunerConfidence = tunerResult.confidence.load();
        uiState.tunerSignalLevel = tunerResult.signalLevel.load();
        if (tunerResult.active.load() && uiState.tunerConfidence > 0.3f) {
            char buf[8];
            std::memcpy(buf, tunerResult.targetNoteName, 8);
            buf[7] = '\0';
            uiState.noteName = buf;
        } else {
            uiState.noteName = "-";
        }

        // Grid data
        GridSnapshot snap = audioEngine->getGridSnapshot();
        uiState.gridNumSteps = snap.numSteps;
        uiState.gridCurrentStep = uiState.playing ? snap.currentStep : -1;
        uiState.subdivision = snap.subdivision;
        uiState.timeSigTop = snap.beatsPerBar;
        std::memcpy(uiState.gridStates, snap.states, 32);

        // Mixer data
        const auto& mixer = audioEngine->getMixer();
        uiState.masterVol = mixer.masterVolume.load();
        uiState.clickVol = mixer.clickVolume.load();
        uiState.accentVol = mixer.accentVolume.load();
        uiState.inputVol = mixer.inputVolume.load();
        uiState.masterPeak = mixer.masterPeak.load();
        uiState.clickPeak = mixer.clickPeak.load();
        uiState.inputPeak = mixer.inputPeak.load();

        // Training data
        uiState.ladderEnabled = audioEngine->isTrainingLadderEnabled();
        uiState.silenceEnabled = audioEngine->isTrainingSilenceEnabled();
        uiState.disappearingEnabled = audioEngine->isTrainingDisappearingEnabled();
        uiState.grooveEnabled = audioEngine->isTrainingGrooveEnabled();
        uiState.drunkenEnabled = audioEngine->isTrainingDrunkenEnabled();
        uiState.bossEnabled = audioEngine->isTrainingBossEnabled();
        uiState.ladderBars = audioEngine->getTrainingLadderBars();
        uiState.ladderInc = audioEngine->getTrainingLadderInc();
        uiState.silenceProb = audioEngine->getTrainingSilenceProb();
        uiState.grooveMaxShiftMs = audioEngine->getTrainingGrooveMaxShiftMs();
        uiState.disappearVisible = audioEngine->getTrainingDisappearVisible();
        uiState.disappearHidden = audioEngine->getTrainingDisappearHidden();
        uiState.drunkenLevel = audioEngine->getTrainingDrunkenLevel();
        uiState.bossLevel = audioEngine->getBossLevel();
        uiState.bossFlash = audioEngine->getBossFlash();
        uiState.bossGameOver = audioEngine->isBossGameOver();
        uiState.humanTestEnabled = audioEngine->isHumanTestEnabled();

        // Polyrhythm (UI state mirrors requested config)
        // Note: stored in AudioEngine atomics; expose via a lightweight heuristic:
        // if enabled, UI will show the last selected ratio.
        // (We keep UI state updated on event handling below.)

        // Mouse
        glfwGetCursorPos(window, &uiState.mouseX, &uiState.mouseY);
        uiState.mouseDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        uiState.mousePressed = uiState.mouseDown && !lastMouseDown;
        lastMouseDown = uiState.mouseDown;

        // --- Render ---
        uiRenderer.newFrame();
        UIEvent event = uiRenderer.render(uiState);

        // --- Handle UI Events ---
        switch (event.type) {
            case UIEventType::TogglePlay:
                audioEngine->setClickEnabled(!uiState.playing);
                break;
            case UIEventType::FreePlayToggle:
                audioEngine->setFreePlayActive(!uiState.freePlay);
                if (!uiState.freePlay) {
                    // entering free play: mute click
                    audioEngine->setClickEnabled(false);
                }
                break;

            case UIEventType::BpmChange:
                audioEngine->setBpm(uiState.bpm + event.value);
                break;

            case UIEventType::TapTempo: {
                double now = glfwGetTime();
                audioEngine->getTuner();  // dummy; tap uses TapDetector
                // Use the TapDetector through a simple timestamp
                // For simplicity: calculate BPM from TAP interval
                static double lastTap = 0;
                static double tapIntervals[4] = {0};
                static int tapCount = 0;
                if (now - lastTap < 3.0 && lastTap > 0) {
                    tapIntervals[tapCount % 4] = now - lastTap;
                    tapCount++;
                    int n = (std::min)(tapCount, 4);
                    double avg = 0;
                    for (int i = 0; i < n; ++i) avg += tapIntervals[i];
                    avg /= n;
                    if (avg > 0.15 && avg < 3.0) {
                        audioEngine->setBpm(60.0 / avg);
                    }
                }
                lastTap = now;
                break;
            }

            case UIEventType::TimeSigChange:
                audioEngine->setTimeSignature(event.intValue);
                audioEngine->getGrid().setBeatsPerBar(event.intValue);
                break;

            case UIEventType::SubdivisionChange:
                audioEngine->setGridSubdivision(event.intValue);
                break;

            case UIEventType::GridStepCycle:
                audioEngine->cycleGridStep(event.intValue);
                break;

            case UIEventType::TrainingToggle: {
                // intValue: 1..4 (see UIRenderer)
                auto id = static_cast<AudioEngine::TrainingModuleId>(event.intValue);
                bool newState = false;
                if (event.intValue == 1) newState = !audioEngine->isTrainingLadderEnabled();
                else if (event.intValue == 2) newState = !audioEngine->isTrainingSilenceEnabled();
                else if (event.intValue == 3) newState = !audioEngine->isTrainingDisappearingEnabled();
                else if (event.intValue == 4) newState = !audioEngine->isTrainingGrooveEnabled();
                else if (event.intValue == 5) newState = !audioEngine->isTrainingDrunkenEnabled();
                else if (event.intValue == 6) newState = !audioEngine->isTrainingBossEnabled();
                audioEngine->setTrainingEnabled(id, newState);
                break;
            }

            case UIEventType::TrainingParamAdjust: {
                auto pid = static_cast<AudioEngine::TrainingParamId>(event.intValue);
                audioEngine->adjustTrainingParam(pid, event.value);
                break;
            }

            case UIEventType::HumanTestToggle:
                audioEngine->setHumanTestEnabled(!uiState.humanTestEnabled);
                break;

            case UIEventType::PolyrhythmSet: {
                // intValue encodes X:Y as XYZ (e.g. 304 => 3:4, 708 => 7:8)
                int code = event.intValue;
                int x = code / 100;
                int y = code % 100;
                audioEngine->setPolyrhythmRatio(x, y);
                uiState.polyEnabled = true;
                uiState.polyX = x;
                uiState.polyY = y;
                break;
            }

            case UIEventType::PolyrhythmClear:
                audioEngine->clearPolyrhythm();
                uiState.polyEnabled = false;
                uiState.polyX = 0;
                uiState.polyY = 0;
                break;

            case UIEventType::TunerModeChange: {
                PreciseTuner::Mode modes[] = {
                    PreciseTuner::Mode::Chromatic,
                    PreciseTuner::Mode::GuitarStandard,
                    PreciseTuner::Mode::GuitarDropD,
                    PreciseTuner::Mode::GuitarDropC,
                    PreciseTuner::Mode::BassStandard,
                    PreciseTuner::Mode::Bass5String,
                    PreciseTuner::Mode::BassBStandard
                };
                if (event.intValue >= 0 && event.intValue < 7) {
                    audioEngine->getTuner().setMode(modes[event.intValue]);
                    uiState.tunerMode = event.intValue;
                }
                break;
            }

            case UIEventType::MixerMasterChange: {
                float v = mixer.masterVolume.load() + event.value;
                if (v < 0) v = 0; if (v > 1) v = 1;
                audioEngine->getMixer().masterVolume.store(v);
                break;
            }
            case UIEventType::MixerClickChange: {
                float v = mixer.clickVolume.load() + event.value;
                if (v < 0) v = 0; if (v > 1) v = 1;
                audioEngine->getMixer().clickVolume.store(v);
                break;
            }
            case UIEventType::MixerAccentChange: {
                float v = mixer.accentVolume.load() + event.value;
                if (v < 0) v = 0; if (v > 1) v = 1;
                audioEngine->getMixer().accentVolume.store(v);
                break;
            }
            case UIEventType::MixerInputChange: {
                float v = mixer.inputVolume.load() + event.value;
                if (v < 0) v = 0; if (v > 1) v = 1;
                audioEngine->getMixer().inputVolume.store(v);
                break;
            }

            default:
                break;
        }

        glfwSwapBuffers(window);
    }

    // 5. Cleanup
    audioEngine->stop();
    settings->save("settings.json");
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
