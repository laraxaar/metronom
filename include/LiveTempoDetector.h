#pragma once
#include <cstdint>
#include <array>
#include <atomic>

/**
 * @brief Real-time BPM tracker based on inter-onset intervals (IOI).
 *
 * Audio thread:
 *  - call onOnsetFrame(absFrame) on each detected onset
 *
 * UI thread:
 *  - call getLiveBpm(), getStabilityIndex()
 *
 * Algorithm:
 *  - keep last N onset times (N=9)
 *  - compute last 4..8 IOIs
 *  - median filter IOI to reject doubled/halved artifacts
 *  - convert to BPM, then smooth BPM with EMA
 *  - stability index based on MAD / median IOI
 */
class LiveTempoDetector {
public:
    void setSampleRate(uint32_t sr) { m_sampleRate.store(sr ? sr : 48000, std::memory_order_relaxed); }
    void reset();

    void onOnsetFrame(uint64_t absFrame);

    double getLiveBpm() const { return m_liveBpm.load(std::memory_order_relaxed); }
    // 0..100 stability where 100 = very stable
    float getStabilityPercent() const { return m_stability.load(std::memory_order_relaxed) * 100.0f; }

private:
    static double median(std::array<double, 8>& v, int n);
    static double mad(std::array<double, 8>& v, int n, double med);

    std::atomic<uint32_t> m_sampleRate{48000};

    // audio-thread ring
    static constexpr int MAX_ONSETS = 9;
    uint64_t m_onsets[MAX_ONSETS]{};
    int m_onsetCount = 0;

    // outputs (atomic)
    std::atomic<double> m_liveBpm{0.0};
    std::atomic<float>  m_stability{0.0f}; // 0..1
};

