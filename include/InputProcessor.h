#pragma once
#include "RingBuffer.h"
#include "DCOffsetFilter.h"
#include "PitchResult.h"
#include <atomic>
#include <cstdint>
#include <vector>

/**
 * @brief Manages the input signal pipeline for pitch analysis.
 *
 * Pipeline: Raw multi-channel input → Channel extraction → DC-offset removal →
 *           Ring buffer storage → Analysis window retrieval
 *
 * The audio callback calls pushSamples() every block.
 * The analysis system (PreciseTuner) calls getAnalysisWindow() to retrieve
 * variable-length windows for YIN/MPM pitch detection.
 *
 * For low-frequency instruments (bass, contrabass), longer windows are needed:
 *   - Low B (30.87 Hz) needs ~3 periods minimum → ~97ms → 4660 samples @ 48kHz
 *   - Recommended window for sub-bass: 8192–16384 samples @ 48kHz
 *
 * Thread-safety: pushSamples() from audio thread, getAnalysisWindow() from any thread.
 */
class InputProcessor {
public:
    /**
     * @brief Construct with ring buffer capacity.
     * @param ringBufferCapacity Size of the internal ring buffer (power-of-two recommended).
     *        Default 131072 = ~2.7 seconds at 48kHz — enough for multiple overlapping windows.
     */
    explicit InputProcessor(size_t ringBufferCapacity = 131072);
    ~InputProcessor() = default;

    /**
     * @brief Initialize with audio parameters.
     * @param sampleRate Sample rate in Hz (48000+ recommended for bass).
     * @param totalInputChannels Total number of input channels from the device.
     */
    void initialize(uint32_t sampleRate, uint32_t totalInputChannels);

    /**
     * @brief Push interleaved multi-channel samples from the audio callback.
     *
     * Extracts the selected input channel, applies DC-offset filter,
     * and writes to the ring buffer. Call this from the audio callback ONLY.
     *
     * @param interleavedData Pointer to interleaved input samples.
     * @param nFrames Number of frames (not samples — each frame has totalChannels samples).
     */
    void pushSamples(const float* interleavedData, uint32_t nFrames);

    /**
     * @brief Push mono (non-interleaved) samples directly.
     * Use when the audio API provides per-channel buffers.
     *
     * @param monoData Pointer to mono input samples (already single-channel).
     * @param nFrames Number of samples.
     */
    void pushMonoSamples(const float* monoData, uint32_t nFrames);

    /**
     * @brief Get an analysis window from the ring buffer without consuming data.
     *
     * Uses peek() — the same data can be read multiple times with overlapping windows.
     * The caller is responsible for calling advanceReadPosition() when done.
     *
     * @param dest Output buffer (must be at least windowSize elements).
     * @param windowSize Desired window size in samples.
     * @return Actual number of samples retrieved (may be < windowSize if buffer underrun).
     */
    size_t getAnalysisWindow(float* dest, size_t windowSize) const;

    /**
     * @brief Get an analysis window with a specific overlap offset.
     * @param dest Output buffer.
     * @param windowSize Window size in samples.
     * @param hopSize Offset from the current read position.
     * @return Actual samples retrieved.
     */
    size_t getAnalysisWindowWithHop(float* dest, size_t windowSize, size_t hopSize) const;

    /**
     * @brief Advance the ring buffer read position by `count` samples.
     * Call after processing an analysis window to move forward.
     * Typical hop size = windowSize / 4 (75% overlap) for YIN/MPM.
     */
    void advanceReadPosition(size_t count);

    /**
     * @brief Check if enough data is available for a given window size.
     * @param windowSize Desired analysis window size.
     * @return true if at least windowSize samples are available.
     */
    bool isWindowAvailable(size_t windowSize) const;

    /**
     * @brief Get the number of samples available in the ring buffer.
     */
    size_t availableSamples() const;

    /**
     * @brief Calculate the recommended window size for a target frequency.
     * For pitch detection, we need at least 2–3 complete periods of the fundamental.
     * @param targetFreqHz Lowest expected frequency (e.g., 30.87 Hz for low B on 5-string bass).
     * @param numPeriods Number of complete periods to capture (default 3 for robustness).
     * @return Recommended window size in samples (rounded up to next power of two).
     */
    size_t recommendedWindowSize(float targetFreqHz, int numPeriods = 3) const;

    /**
     * @brief Set the input channel to extract from interleaved data.
     * @param channel 0-based channel index.
     */
    void setInputChannel(uint32_t channel);

    /**
     * @brief Get the currently selected input channel.
     */
    uint32_t getInputChannel() const { return m_inputChannel.load(std::memory_order_relaxed); }

    /**
     * @brief Get the shared PitchResult for reading from GUI thread.
     */
    PitchResult& getPitchResult() { return m_pitchResult; }
    const PitchResult& getPitchResult() const { return m_pitchResult; }

    /**
     * @brief Get the current input RMS level (for VU meter / noise gate).
     */
    float getCurrentRMS() const { return m_currentRMS.load(std::memory_order_relaxed); }

    /**
     * @brief Get peak input level.
     */
    float getPeakLevel() const { return m_peakLevel.load(std::memory_order_relaxed); }

    /**
     * @brief Reset all state (call when stopping the stream).
     */
    void reset();

private:
    RingBuffer<float>   m_ringBuffer;
    DCOffsetFilter      m_dcFilter;
    PitchResult         m_pitchResult;

    uint32_t            m_sampleRate{48000};
    uint32_t            m_totalInputChannels{1};
    std::atomic<uint32_t> m_inputChannel{0};

    // Metering (written from audio thread, read from GUI)
    std::atomic<float>  m_currentRMS{0.0f};
    std::atomic<float>  m_peakLevel{0.0f};

    // Temporary buffer for channel extraction (avoids allocation in callback)
    std::vector<float>  m_channelExtractBuffer;
};
