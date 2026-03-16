#include "IMetronomeModule.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <atomic>

/**
 * @brief Evaluation results for hit accuracy.
 */
enum class AccuracyGrade {
    Perfect,
    Early,
    Late,
    None
};

/**
 * @brief Plugin that analyzes input audio onsets and compares them with metronome beats.
 */
class AccuracyCalculator : public IMetronomeModule {
public:
    void onInitialize(const AudioParams& params) override {
        m_sampleRate = params.sampleRate;
    }

    void processAudio(const float* input, float* output, uint32_t nFrames) override {
        // Simple RMS-based onset detection for this example
        float sum2 = 0.0f;
        for (uint32_t i = 0; i < nFrames; ++i) {
            sum2 += input[i] * input[i];
        }
        float rms = std::sqrt(sum2 / static_cast<float>(nFrames));

        if (rms > m_threshold && (m_totalFrames - m_lastHitFrame) > (m_sampleRate * 0.1)) { // 100ms debounce
            m_lastHitFrame = m_totalFrames;
            evaluateHit();
        }

        m_totalFrames += nFrames;
    }

    void onBeat(int beatIndex, uint32_t sampleOffset) override {
        m_lastBeatFrame = m_totalFrames + sampleOffset;
    }

    void onBar(int barIndex) override {
        // Not used for simple accuracy
    }

    void onConfigChange(const std::string& jsonConfig) override {
        // Parse threshold from JSON here
    }

    const char* getName() const override { return "AccuracyCalculator"; }

private:
    uint32_t m_sampleRate = 44100;
    uint64_t m_totalFrames = 0;
    uint64_t m_lastBeatFrame = 0;
    uint64_t m_lastHitFrame = 0;
    float m_threshold = 0.1f;

    void evaluateHit() {
        int64_t diffFrames = static_cast<int64_t>(m_lastHitFrame) - static_cast<int64_t>(m_lastBeatFrame);
        double diffMs = (static_cast<double>(diffFrames) / m_sampleRate) * 1000.0;

        AccuracyGrade grade = AccuracyGrade::None;
        if (std::abs(diffMs) < 15.0) {
            grade = AccuracyGrade::Perfect;
        } else if (diffMs < 0) {
            grade = AccuracyGrade::Early;
        } else {
            grade = AccuracyGrade::Late;
        }

        std::cout << "Hit detected: " << diffMs << " ms (" 
                  << (grade == AccuracyGrade::Perfect ? "Perfect" : 
                     (grade == AccuracyGrade::Early ? "Early" : "Late")) 
                  << ")" << std::endl;
    }
};
