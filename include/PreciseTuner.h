#pragma once
#include <vector>
#include <atomic>
#include <cstdint>
#include <string>
#include <cstring>

/**
 * @brief Thread-safe tuner result structure.
 * Written by the TunerWorker thread, read by GUI. All fields are atomic.
 */
struct TunerResult {
    std::atomic<float>    currentFreq{0.0f};     ///< Detected fundamental frequency (Hz)
    std::atomic<float>    diffCents{0.0f};       ///< Deviation from target in cents (-50..+50)
    std::atomic<float>    signalLevel{0.0f};     ///< RMS signal level (for silence gating)
    std::atomic<float>    confidence{0.0f};      ///< Detection confidence (0..1)
    std::atomic<int>      targetNoteIndex{-1};   ///< Index in preset, or MIDI note for chromatic
    std::atomic<bool>     active{false};         ///< Whether a valid pitch is being tracked

    // Lock-free note name: small fixed array, written atomically via memcpy
    // Max 7 chars + null: "C#3", "Bb1", "E2", etc.
    char targetNoteName[8]{};

    void setNoteName(const char* name) {
        char buf[8] = {};
        if (name) {
            size_t len = std::strlen(name);
            if (len > 7) len = 7;
            std::memcpy(buf, name, len);
        }
        std::memcpy(targetNoteName, buf, 8);  // atomic on most archs for 8 bytes
    }

    void reset() {
        currentFreq.store(0.0f, std::memory_order_relaxed);
        diffCents.store(0.0f, std::memory_order_relaxed);
        signalLevel.store(0.0f, std::memory_order_relaxed);
        confidence.store(0.0f, std::memory_order_relaxed);
        targetNoteIndex.store(-1, std::memory_order_relaxed);
        active.store(false, std::memory_order_relaxed);
        std::memset(targetNoteName, 0, 8);
    }

    TunerResult() = default;
    TunerResult(const TunerResult&) = delete;
    TunerResult& operator=(const TunerResult&) = delete;
};

/**
 * @brief Represents a specific instrument tuning preset.
 */
struct TuningPreset {
    std::string name;
    std::vector<float> frequencies;
    std::vector<std::string> noteNames;  ///< Note names corresponding to frequencies
};

/**
 * @brief High-precision Pitch Detection with multiple algorithms and modes.
 *
 * Supports:
 *   - Chromatic mode (12-TET, all semitones)
 *   - Instrument presets (Guitar E/DropD/DropC, Bass E/B-Standard, 5-string)
 *   - YIN algorithm with cumulative mean normalized difference function
 *   - MPM (McLeod Pitch Method) for comparison/fallback
 *   - Dynamic window sizing for bass (<60 Hz → 8192 samples)
 *   - 4-pole Butterworth LPF (500 Hz for bass, 1200 Hz for guitar)
 *
 * Thread-safety: process() is called from TunerWorker thread, results are atomic.
 */
class PreciseTuner {
public:
    enum class Mode {
        Chromatic,         ///< 12-TET, all semitones
        GuitarStandard,    ///< E A D G B E
        GuitarDropD,       ///< D A D G B E
        GuitarDropC,       ///< C G C F A D
        BassStandard,      ///< E A D G (4-string)
        Bass5String,       ///< B E A D G (5-string)
        BassBStandard,     ///< B F# B E (4-string B standard)
        Custom
    };

    PreciseTuner();
    ~PreciseTuner() = default;

    /**
     * @brief Initialize the tuner with sample rate.
     */
    void initialize(uint32_t sampleRate);

    /**
     * @brief Process a block of input samples and detect pitch.
     * Called from TunerWorker thread (NOT audio callback).
     */
    void process(const float* input, uint32_t nFrames);

    /**
     * @brief Set the tuner mode and update internal filters/presets.
     */
    void setMode(Mode mode);

    /**
     * @brief Set a custom tuning target.
     */
    void setCustomTuning(const std::vector<float>& frequencies);

    /**
     * @brief Set custom tuning with note names.
     */
    void setCustomTuning(const std::vector<float>& frequencies, const std::vector<std::string>& names);

    /**
     * @brief Enable/disable the tuner.
     */
    void setEnabled(bool enabled) { m_enabled = enabled; }

    // ----- Thread-safe result access -----
    const TunerResult& getResult() const { return m_result; }
    float getFrequency() const { return m_result.currentFreq.load(); }
    float getConfidence() const { return m_result.confidence.load(); }
    float getCentsDeviation() const { return m_result.diffCents.load(); }
    int getNearestNoteIndex() const { return m_result.targetNoteIndex.load(); }
    float getSignalLevel() const { return m_result.signalLevel.load(); }

    /**
     * @brief Get the current dynamic window size based on detected frequency.
     * For bass (<60 Hz): 8192+ samples. For guitar: 4096. Chromatic: 4096.
     */
    size_t getCurrentWindowSize() const { return m_activeWindowSize; }

    /**
     * @brief Get the recommended minimum window size for the current mode.
     */
    size_t getMinWindowSize() const;

    /**
     * @brief Get the current tuning preset.
     */
    const TuningPreset& getCurrentPreset() const { return m_currentPreset; }

private:
    uint32_t m_sampleRate = 48000;
    bool m_enabled = true;
    Mode m_mode = Mode::Chromatic;
    TuningPreset m_currentPreset;
    TunerResult m_result;

    // Analysis buffers
    std::vector<float> m_buffer;
    size_t m_cursor = 0;
    size_t m_windowSize = 4096;       ///< Base window size
    size_t m_activeWindowSize = 4096; ///< Dynamically adjusted window size

    // 4-pole Butterworth LPF state
    struct BiquadState {
        float x1 = 0, x2 = 0;
        float y1 = 0, y2 = 0;
    };
    struct BiquadCoeffs {
        float b0 = 1, b1 = 0, b2 = 0;
        float a1 = 0, a2 = 0;
    };
    BiquadState  m_lpfState[2];   // 2 cascaded biquads = 4-pole
    BiquadCoeffs m_lpfCoeffs[2];
    bool m_lpfEnabled = true;
    float m_lpfCutoff = 500.0f;

    // Signal thresholds
    static constexpr float SILENCE_THRESHOLD = 0.005f;  // RMS below this = silence
    static constexpr float YIN_THRESHOLD = 0.10f;       // YIN confidence threshold

    // ----- Internal algorithms -----

    /**
     * @brief YIN pitch detection algorithm.
     * Uses cumulative mean normalized difference function (CMNDF).
     * More accurate than basic autocorrelation for monophonic signals.
     */
    float computeYIN(const float* data, size_t windowSize, float& outConfidence);

    /**
     * @brief McLeod Pitch Method (MPM) — fallback/comparison.
     */
    float computeMPM(const float* data, size_t windowSize, float& outConfidence);

    /**
     * @brief Dynamically compute window size based on detected or expected frequency.
     * For f < 60 Hz: up to 8192 samples. For f > 200 Hz: 2048 samples.
     */
    size_t computeDynamicWindowSize(float detectedFreq) const;

    /**
     * @brief Snap frequency to nearest chromatic note (12-TET, A440).
     * Sets targetNoteIndex to MIDI note number, targetNoteName to "C4", "F#2", etc.
     */
    void chromaticSnap(float frequency);

    /**
     * @brief Snap frequency to nearest note in the current preset.
     * Ignores harmonics by rejecting detections >200 cents from any target.
     */
    void presetSnap(float frequency);

    /**
     * @brief Apply the 4-pole Butterworth LPF to a buffer in-place.
     */
    void applyLPF(float* buffer, size_t count);

    /**
     * @brief Recalculate Butterworth LPF coefficients for a given cutoff frequency.
     */
    void computeLPFCoefficients(float cutoffHz);

    /**
     * @brief Reset LPF state (call on mode change or stream restart).
     */
    void resetLPF();

    /**
     * @brief Get note name from MIDI note number.
     */
    static const char* midiToNoteName(int midiNote);
};
