#include "TunerWorker.h"
#include <iostream>
#include <chrono>

TunerWorker::TunerWorker() {
    m_analysisBuffer.resize(MAX_WINDOW_SIZE, 0.0f);
}

TunerWorker::~TunerWorker() {
    stop();
}

void TunerWorker::initialize(InputProcessor* inputProcessor, PreciseTuner* tuner, uint32_t sampleRate) {
    m_inputProcessor = inputProcessor;
    m_tuner = tuner;
    m_sampleRate = sampleRate;
}

void TunerWorker::start() {
    if (m_running.load()) return;
    if (!m_inputProcessor || !m_tuner) {
        std::cerr << "[TunerWorker] Cannot start: InputProcessor or Tuner not set." << std::endl;
        return;
    }

    m_running.store(true);
    m_thread = std::thread(&TunerWorker::workerLoop, this);

    std::cout << "[TunerWorker] Started pitch analysis thread." << std::endl;
}

void TunerWorker::stop() {
    if (!m_running.load()) return;

    m_running.store(false);

    // Wake up the thread so it can exit
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hasNewData.store(true);
    }
    m_cv.notify_one();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    std::cout << "[TunerWorker] Stopped pitch analysis thread." << std::endl;
}

void TunerWorker::notifyNewData() {
    // This is called from the audio callback — must be fast.
    // We use a simple atomic flag + notify.
    // The mutex lock here is acceptable because the worker thread
    // holds it only very briefly during the wait condition check.
    m_hasNewData.store(true, std::memory_order_release);
    m_cv.notify_one();
}

void TunerWorker::workerLoop() {
    while (m_running.load(std::memory_order_relaxed)) {
        // Wait for notification of new data
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return m_hasNewData.load(std::memory_order_acquire) || !m_running.load(std::memory_order_relaxed);
            });
            m_hasNewData.store(false, std::memory_order_relaxed);
        }

        if (!m_running.load(std::memory_order_relaxed)) break;
        if (!m_enabled.load(std::memory_order_relaxed)) continue;

        // Get the required window size from the tuner
        size_t windowSize = m_tuner->getCurrentWindowSize();
        if (windowSize > MAX_WINDOW_SIZE) windowSize = MAX_WINDOW_SIZE;
        if (windowSize < 1024) windowSize = 1024;

        // Check if enough data is available in the ring buffer
        if (!m_inputProcessor->isWindowAvailable(windowSize)) continue;

        // Peek at the analysis window (non-consuming read)
        size_t got = m_inputProcessor->getAnalysisWindow(m_analysisBuffer.data(), windowSize);
        if (got < windowSize) continue;

        // Run pitch detection
        m_tuner->process(m_analysisBuffer.data(), static_cast<uint32_t>(got));

        // Advance ring buffer read position by hop size (25% of window = 75% overlap)
        size_t hopSize = windowSize / 4;
        m_inputProcessor->advanceReadPosition(hopSize);
    }
}
