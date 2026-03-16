#pragma once
#include <vector>
#include <atomic>
#include <cstdint>
#include <string>
#include <optional>

/**
 * @brief Represents a specific instrument tuning.
 */
struct TuningPreset {
    std::string name;
    std::vector<float> frequencies; // e.g., 82.41, 110.0, etc.
};

/**
 * @brief High-precision Pitch Detection based on MPM (McLeod Pitch Method).
 * Optimized for Bass, Guitar (Hi-Gain), and Drums.
 */
class PreciseTuner {
public:
    enum class Mode {
        GuitarStandard,
        GuitarDropD,
        BassStandard,
        Bass5String,
        Custom
    };

    PreciseTuner();
    ~PreciseTuner() = default;

    /**
     * @brief Initialize the tuner with sample rate.
     */
    void initialize(uint32_t sampleRate);

    /**
     * @brief Process a block of input samples.
     */
    void process(const float* input, uint32_t nFrames);

    /**
     * @brief Set the tuner mode and update internal filters.
     */
    void setMode(Mode mode);
    
    /**
     * @brief Set a custom tuning target.
     */
    void setCustomTuning(const std::vector<float>& frequencies);

    /**
     * @brief Enable/disable the tuner.
     */
    void setEnabled(bool enabled) { m_enabled = enabled; }

    // Results (Thread-safe)
    float getFrequency() const { return m_currentHz.load(); }
    float getConfidence() const { return m_confidence.load(); }
    
    /**
     * @brief Returns the index of the nearest note in the current tuning.
     */
    int getNearestNoteIndex() const;
    
    /**
     * @brief Returns the deviation in cents from the nearest target note.
     */
    float getCentsDeviation() const;

private:
    uint32_t m_sampleRate = 44100;
    bool m_enabled = true;
    Mode m_mode = Mode::GuitarStandard;
    std::vector<float> m_targetFrequencies;

    // Buffers and indices
    std::vector<float> m_buffer;
    size_t m_cursor = 0;
    size_t m_windowSize = 8192;

    std::atomic<float> m_currentHz{0.0f};
    std::atomic<float> m_confidence{0.0f};

    // DSP State
    float m_lpfState = 0.0f;
    float m_lpfAlpha = 0.15f; 

    /**
     * @brief McLeod Pitch Method implementation.
     * More robust than YIN for low frequencies and harmonic-rich signals.
     */
    float computeMPM(float& outConfidence);
};
