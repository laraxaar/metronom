#pragma once
#include "InputProcessor.h"
#include "PreciseTuner.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

/**
 * @brief Dedicated worker thread for pitch analysis.
 *
 * Decouples pitch detection from the audio callback:
 *   - Audio callback writes samples to InputProcessor's RingBuffer
 *   - TunerWorker wakes up when enough data is available
 *   - Runs PreciseTuner::process() on a variable-length window
 *   - Results are available via TunerResult (atomic, lock-free reads)
 *
 * This prevents YIN/MPM computation (~O(N²) for window size N) from
 * blocking the audio callback, especially with large windows for bass.
 */
class TunerWorker {
public:
    TunerWorker();
    ~TunerWorker();

    /**
     * @brief Initialize with references to the input processor and tuner.
     * Must be called before start().
     */
    void initialize(InputProcessor* inputProcessor, PreciseTuner* tuner, uint32_t sampleRate);

    /**
     * @brief Start the worker thread.
     */
    void start();

    /**
     * @brief Stop the worker thread and join.
     */
    void stop();

    /**
     * @brief Notify the worker that new audio data is available.
     * Called from the audio callback (lock-free).
     */
    void notifyNewData();

    /**
     * @brief Enable/disable tuner processing.
     */
    void setEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

    /**
     * @brief Check if the worker thread is running.
     */
    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }

private:
    void workerLoop();

    InputProcessor*  m_inputProcessor = nullptr;
    PreciseTuner*    m_tuner = nullptr;
    uint32_t         m_sampleRate = 48000;

    std::thread              m_thread;
    std::atomic<bool>        m_running{false};
    std::atomic<bool>        m_enabled{true};
    std::atomic<bool>        m_hasNewData{false};

    std::mutex               m_mutex;
    std::condition_variable  m_cv;

    // Pre-allocated analysis buffer (max window size)
    std::vector<float>       m_analysisBuffer;
    static constexpr size_t  MAX_WINDOW_SIZE = 16384;
};
