#include "LiveTempoDetector.h"
#include <algorithm>
#include <cmath>

void LiveTempoDetector::reset() {
    m_onsetCount = 0;
    for (int i = 0; i < MAX_ONSETS; ++i) m_onsets[i] = 0;
    m_liveBpm.store(0.0, std::memory_order_relaxed);
    m_stability.store(0.0f, std::memory_order_relaxed);
}

static double clampBpm(double bpm) {
    if (!std::isfinite(bpm)) return 0.0;
    if (bpm < 10.0) bpm = 10.0;
    if (bpm > 300.0) bpm = 300.0; // free play: practical range
    return bpm;
}

double LiveTempoDetector::median(std::array<double, 8>& v, int n) {
    if (n <= 0) return 0.0;
    std::nth_element(v.begin(), v.begin() + n / 2, v.begin() + n);
    const double m = v[n / 2];
    if ((n & 1) == 1) return m;
    // for even n, approximate by averaging two middle values
    auto it = std::max_element(v.begin(), v.begin() + n / 2);
    return 0.5 * (m + *it);
}

double LiveTempoDetector::mad(std::array<double, 8>& v, int n, double med) {
    for (int i = 0; i < n; ++i) v[i] = std::abs(v[i] - med);
    return median(v, n);
}

void LiveTempoDetector::onOnsetFrame(uint64_t absFrame) {
    if (absFrame == 0) return;

    // Insert onset (shift left if full)
    if (m_onsetCount < MAX_ONSETS) {
        m_onsets[m_onsetCount++] = absFrame;
    } else {
        for (int i = 1; i < MAX_ONSETS; ++i) m_onsets[i - 1] = m_onsets[i];
        m_onsets[MAX_ONSETS - 1] = absFrame;
    }

    if (m_onsetCount < 5) return; // need at least 4 intervals

    const uint32_t sr = m_sampleRate.load(std::memory_order_relaxed);
    if (sr == 0) return;

    // Compute last 4..8 IOIs
    const int intervals = std::min(8, m_onsetCount - 1);
    std::array<double, 8> ioi{};
    int n = 0;
    for (int i = m_onsetCount - intervals; i < m_onsetCount; ++i) {
        const uint64_t a = m_onsets[i - 1];
        const uint64_t b = m_onsets[i];
        if (b <= a) continue;
        const double dt = static_cast<double>(b - a) / static_cast<double>(sr);
        // Reject absurd intervals (<60ms or >2.5s)
        if (dt < 0.06 || dt > 2.5) continue;
        ioi[n++] = dt;
    }
    if (n < 4) return;

    // Median IOI for robustness
    std::array<double, 8> tmp = ioi;
    const double medIoi = median(tmp, n);
    if (medIoi <= 0.0) return;

    // Convert to BPM
    double bpm = 60.0 / medIoi;

    // Anti-doubling/halving: fold into neighborhood of previous BPM if available
    const double prev = m_liveBpm.load(std::memory_order_relaxed);
    if (prev > 10.0) {
        // Try bpm, bpm/2, bpm*2 and pick closest to prev
        double cands[3] = { bpm, bpm * 0.5, bpm * 2.0 };
        double best = cands[0];
        double bestErr = std::abs(cands[0] - prev);
        for (int i = 1; i < 3; ++i) {
            const double e = std::abs(cands[i] - prev);
            if (e < bestErr) { bestErr = e; best = cands[i]; }
        }
        bpm = best;
    }

    bpm = clampBpm(bpm);

    // Smooth BPM (EMA)
    const double alpha = 0.18;
    const double smoothed = (prev <= 0.0) ? bpm : (prev + (bpm - prev) * alpha);
    m_liveBpm.store(smoothed, std::memory_order_relaxed);

    // Stability: MAD / median
    std::array<double, 8> tmp2 = ioi;
    const double madIoi = mad(tmp2, n, medIoi);
    const double rel = (medIoi > 0.0) ? (madIoi / medIoi) : 1.0;
    // Map relative variability to 0..1 stability (0 = chaos, 1 = stable)
    // rel ~0.00..0.02 => stable, rel >=0.08 => chaos
    double stab = 1.0 - std::clamp((rel - 0.01) / 0.07, 0.0, 1.0);
    m_stability.store(static_cast<float>(stab), std::memory_order_relaxed);
}

