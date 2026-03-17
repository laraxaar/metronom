#include "PreciseTuner.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// =====================================================================
// Note name table
// =====================================================================
static const char* NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// Static buffer for midiToNoteName (thread-local for safety)
static thread_local char s_noteNameBuf[8];

const char* PreciseTuner::midiToNoteName(int midiNote) {
    if (midiNote < 0 || midiNote > 127) {
        s_noteNameBuf[0] = '?';
        s_noteNameBuf[1] = '\0';
        return s_noteNameBuf;
    }
    int noteIdx = midiNote % 12;
    int octave  = (midiNote / 12) - 1;
    snprintf(s_noteNameBuf, sizeof(s_noteNameBuf), "%s%d", NOTE_NAMES[noteIdx], octave);
    return s_noteNameBuf;
}

// =====================================================================
// Construction & Initialization
// =====================================================================

PreciseTuner::PreciseTuner() {
    setMode(Mode::Chromatic);
}

void PreciseTuner::initialize(uint32_t sampleRate) {
    m_sampleRate = sampleRate;
    m_windowSize = 4096;
    m_activeWindowSize = m_windowSize;
    m_buffer.assign(16384 * 2, 0.0f);  // Max buffer for dynamic windowing
    m_cursor = 0;
    resetLPF();
    setMode(m_mode);  // Recalculate LPF coefficients with new sample rate
}

// =====================================================================
// Mode & Preset Configuration
// =====================================================================

void PreciseTuner::setMode(Mode mode) {
    m_mode = mode;

    switch (mode) {
        case Mode::Chromatic:
            m_currentPreset = {"Chromatic", {}, {}};
            m_lpfEnabled = false;  // No LPF in chromatic mode
            m_lpfCutoff = 4000.0f;
            m_windowSize = 4096;
            break;

        case Mode::GuitarStandard:
            m_currentPreset = {"Guitar E Standard",
                {82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f},
                {"E2", "A2", "D3", "G3", "B3", "E4"}};
            m_lpfEnabled = true;
            m_lpfCutoff = 1200.0f;
            m_windowSize = 4096;
            break;

        case Mode::GuitarDropD:
            m_currentPreset = {"Guitar Drop D",
                {73.42f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f},
                {"D2", "A2", "D3", "G3", "B3", "E4"}};
            m_lpfEnabled = true;
            m_lpfCutoff = 1200.0f;
            m_windowSize = 4096;
            break;

        case Mode::GuitarDropC:
            m_currentPreset = {"Guitar Drop C",
                {65.41f, 98.00f, 130.81f, 174.61f, 220.00f, 293.66f},
                {"C2", "G2", "C3", "F3", "A3", "D4"}};
            m_lpfEnabled = true;
            m_lpfCutoff = 1000.0f;
            m_windowSize = 4096;
            break;

        case Mode::BassStandard:
            m_currentPreset = {"Bass E Standard",
                {41.20f, 55.00f, 73.42f, 98.00f},
                {"E1", "A1", "D2", "G2"}};
            m_lpfEnabled = true;
            m_lpfCutoff = 500.0f;
            m_windowSize = 8192;
            break;

        case Mode::Bass5String:
            m_currentPreset = {"Bass 5-String B",
                {30.87f, 41.20f, 55.00f, 73.42f, 98.00f},
                {"B0", "E1", "A1", "D2", "G2"}};
            m_lpfEnabled = true;
            m_lpfCutoff = 500.0f;
            m_windowSize = 8192;
            break;

        case Mode::BassBStandard:
            m_currentPreset = {"Bass B Standard",
                {30.87f, 46.25f, 61.74f, 82.41f},
                {"B0", "F#1", "B1", "E2"}};
            m_lpfEnabled = true;
            m_lpfCutoff = 500.0f;
            m_windowSize = 8192;
            break;

        case Mode::Custom:
            m_lpfEnabled = true;
            m_lpfCutoff = 1500.0f;
            m_windowSize = 4096;
            break;
    }

    m_activeWindowSize = m_windowSize;

    if (m_lpfEnabled && m_sampleRate > 0) {
        computeLPFCoefficients(m_lpfCutoff);
    }
    resetLPF();
}

void PreciseTuner::setCustomTuning(const std::vector<float>& frequencies) {
    m_currentPreset.name = "Custom";
    m_currentPreset.frequencies = frequencies;
    m_currentPreset.noteNames.clear();
    m_mode = Mode::Custom;

    // Determine if this is a bass tuning based on lowest frequency
    if (!frequencies.empty() && frequencies[0] < 60.0f) {
        m_lpfCutoff = 500.0f;
        m_windowSize = 8192;
    } else {
        m_lpfCutoff = 1500.0f;
        m_windowSize = 4096;
    }
    m_activeWindowSize = m_windowSize;
    m_lpfEnabled = true;
    if (m_sampleRate > 0) computeLPFCoefficients(m_lpfCutoff);
    resetLPF();
}

void PreciseTuner::setCustomTuning(const std::vector<float>& frequencies, const std::vector<std::string>& names) {
    setCustomTuning(frequencies);
    m_currentPreset.noteNames = names;
}

size_t PreciseTuner::getMinWindowSize() const {
    switch (m_mode) {
        case Mode::BassStandard:
        case Mode::Bass5String:
        case Mode::BassBStandard:
            return 8192;
        default:
            return 4096;
    }
}

// =====================================================================
// Main Processing (called from TunerWorker, NOT audio callback)
// =====================================================================

void PreciseTuner::process(const float* input, uint32_t nFrames) {
    if (!m_enabled || !input) return;

    for (uint32_t i = 0; i < nFrames; ++i) {
        float sample = input[i];

        // Apply 4-pole Butterworth LPF if enabled
        if (m_lpfEnabled) {
            // Pass through two cascaded biquad sections
            for (int section = 0; section < 2; ++section) {
                auto& s = m_lpfState[section];
                auto& c = m_lpfCoeffs[section];
                float y = c.b0 * sample + c.b1 * s.x1 + c.b2 * s.x2
                                        - c.a1 * s.y1 - c.a2 * s.y2;
                s.x2 = s.x1;
                s.x1 = sample;
                s.y2 = s.y1;
                s.y1 = y;
                sample = y;
            }
        }

        m_buffer[m_cursor++] = sample;

        if (m_cursor >= m_activeWindowSize) {
            // Calculate RMS of the window
            float sumSq = 0.0f;
            for (size_t j = 0; j < m_activeWindowSize; ++j) {
                sumSq += m_buffer[j] * m_buffer[j];
            }
            float rms = std::sqrt(sumSq / static_cast<float>(m_activeWindowSize));
            m_result.signalLevel.store(rms, std::memory_order_relaxed);

            // Gate silence
            if (rms < SILENCE_THRESHOLD) {
                m_result.active.store(false, std::memory_order_relaxed);
                // Fade confidence
                float prevConf = m_result.confidence.load(std::memory_order_relaxed);
                m_result.confidence.store(prevConf * 0.85f, std::memory_order_relaxed);
            } else {
                // Run YIN pitch detection
                float yinConf = 0.0f;
                float yinHz = computeYIN(m_buffer.data(), m_activeWindowSize, yinConf);

                // If YIN gave poor result, try MPM as fallback
                float finalHz = yinHz;
                float finalConf = yinConf;

                if (yinConf < 0.5f) {
                    float mpmConf = 0.0f;
                    float mpmHz = computeMPM(m_buffer.data(), m_activeWindowSize, mpmConf);
                    if (mpmConf > yinConf) {
                        finalHz = mpmHz;
                        finalConf = mpmConf;
                    }
                }

                if (finalHz > 15.0f && finalHz < 5000.0f && finalConf > 0.3f) {
                    m_result.currentFreq.store(finalHz, std::memory_order_relaxed);
                    m_result.confidence.store(finalConf, std::memory_order_relaxed);
                    m_result.active.store(true, std::memory_order_relaxed);

                    // Snap to note based on mode
                    if (m_mode == Mode::Chromatic) {
                        chromaticSnap(finalHz);
                    } else {
                        presetSnap(finalHz);
                    }

                    // Dynamically adjust window size based on detected frequency
                    size_t newWindow = computeDynamicWindowSize(finalHz);
                    if (newWindow != m_activeWindowSize) {
                        m_activeWindowSize = newWindow;
                    }
                } else {
                    m_result.active.store(false, std::memory_order_relaxed);
                    float prevConf = m_result.confidence.load(std::memory_order_relaxed);
                    m_result.confidence.store(prevConf * 0.9f, std::memory_order_relaxed);
                }
            }

            // Overlap: shift by 25% (75% overlap)
            size_t hop = m_activeWindowSize / 4;
            if (hop > 0 && m_cursor >= hop) {
                std::memmove(m_buffer.data(), m_buffer.data() + hop,
                    (m_buffer.size() - hop) * sizeof(float));
                m_cursor -= hop;
            } else {
                m_cursor = 0;
            }
        }
    }
}

// =====================================================================
// YIN Algorithm (Cumulative Mean Normalized Difference Function)
// =====================================================================

float PreciseTuner::computeYIN(const float* data, size_t windowSize, float& outConfidence) {
    const size_t halfW = windowSize / 2;
    std::vector<float> diff(halfW, 0.0f);

    // Step 1: Difference function d(tau) = sum((x[i] - x[i+tau])^2)
    for (size_t tau = 0; tau < halfW; ++tau) {
        float sum = 0.0f;
        for (size_t i = 0; i < halfW; ++i) {
            float delta = data[i] - data[i + tau];
            sum += delta * delta;
        }
        diff[tau] = sum;
    }

    // Step 2: Cumulative mean normalized difference function (CMNDF)
    // d'(0) = 1, d'(tau) = d(tau) / ((1/tau) * sum(d(j), j=1..tau))
    diff[0] = 1.0f;
    float runningSum = 0.0f;
    for (size_t tau = 1; tau < halfW; ++tau) {
        runningSum += diff[tau];
        if (runningSum > 0.0f) {
            diff[tau] = diff[tau] * static_cast<float>(tau) / runningSum;
        } else {
            diff[tau] = 1.0f;
        }
    }

    // Step 3: Absolute threshold — find first tau where d'(tau) < threshold
    size_t tauMin = 2;  // Skip tau=0,1 (too short for any musical pitch)
    // Set minimum tau based on max detectable frequency (~5000 Hz)
    // and maximum tau based on min detectable frequency (~20 Hz)
    size_t tauMinPitch = (m_sampleRate > 0) ? (m_sampleRate / 5000) : 2;
    size_t tauMaxPitch = (m_sampleRate > 0) ? std::min(halfW, (size_t)(m_sampleRate / 20)) : halfW;
    if (tauMinPitch < 2) tauMinPitch = 2;

    size_t bestTau = 0;
    for (size_t tau = tauMinPitch; tau < tauMaxPitch; ++tau) {
        if (diff[tau] < YIN_THRESHOLD) {
            // Find the local minimum after crossing the threshold
            while (tau + 1 < tauMaxPitch && diff[tau + 1] < diff[tau]) {
                ++tau;
            }
            bestTau = tau;
            break;
        }
    }

    // If no dip below threshold, find the global minimum
    if (bestTau == 0) {
        float minVal = 1e6f;
        for (size_t tau = tauMinPitch; tau < tauMaxPitch; ++tau) {
            if (diff[tau] < minVal) {
                minVal = diff[tau];
                bestTau = tau;
            }
        }
        // Only accept if the minimum is reasonably low
        if (minVal > 0.5f) {
            outConfidence = 0.0f;
            return 0.0f;
        }
    }

    if (bestTau == 0) {
        outConfidence = 0.0f;
        return 0.0f;
    }

    // Step 4: Parabolic interpolation for sub-sample accuracy
    float refinedTau = static_cast<float>(bestTau);
    if (bestTau > 0 && bestTau < halfW - 1) {
        float s0 = diff[bestTau - 1];
        float s1 = diff[bestTau];
        float s2 = diff[bestTau + 1];
        float denom = s0 + s2 - 2.0f * s1;
        if (std::abs(denom) > 1e-6f) {
            refinedTau += (s0 - s2) / (2.0f * denom);
        }
    }

    outConfidence = 1.0f - diff[bestTau];
    if (outConfidence < 0.0f) outConfidence = 0.0f;
    if (outConfidence > 1.0f) outConfidence = 1.0f;

    if (refinedTau <= 0.0f) return 0.0f;
    return static_cast<float>(m_sampleRate) / refinedTau;
}

// =====================================================================
// MPM (McLeod Pitch Method) — fallback
// =====================================================================

float PreciseTuner::computeMPM(const float* data, size_t windowSize, float& outConfidence) {
    const size_t W = windowSize / 2;
    std::vector<float> nsdf(W, 0.0f);

    // NSDF using autocorrelation
    for (size_t tau = 0; tau < W; ++tau) {
        float ac = 0.0f;
        float m = 0.0f;
        for (size_t i = 0; i < W; ++i) {
            float x = data[i];
            float y = data[i + tau];
            ac += x * y;
            m += x * x + y * y;
        }
        nsdf[tau] = 2.0f * ac / (m + 1e-6f);
    }

    // Peak picking
    size_t start = 0;
    while (start < W - 1 && nsdf[start] > 0) start++;

    std::vector<size_t> peaks;
    float maxVal = -1.0f;

    for (size_t tau = std::max((size_t)1, start); tau < W - 1; ++tau) {
        if (nsdf[tau] > 0 && nsdf[tau] > nsdf[tau-1] && nsdf[tau] > nsdf[tau+1]) {
            peaks.push_back(tau);
            if (nsdf[tau] > maxVal) maxVal = nsdf[tau];
        }
    }

    if (peaks.empty() || maxVal < 0.3f) {
        outConfidence = 0.0f;
        return 0.0f;
    }

    size_t bestTau = 0;
    for (size_t p : peaks) {
        if (nsdf[p] >= 0.85f * maxVal) {
            bestTau = p;
            break;
        }
    }

    outConfidence = nsdf[bestTau];

    // Parabolic interpolation
    if (bestTau > 0 && bestTau < W - 1) {
        float s0 = nsdf[bestTau - 1];
        float s1 = nsdf[bestTau];
        float s2 = nsdf[bestTau + 1];
        float den = s0 + s2 - 2.0f * s1;
        float shift = (std::abs(den) < 1e-6f) ? 0.0f : (s0 - s2) / (2.0f * den);
        return static_cast<float>(m_sampleRate) / (static_cast<float>(bestTau) + shift);
    }

    return static_cast<float>(m_sampleRate) / static_cast<float>(bestTau);
}

// =====================================================================
// Dynamic Window Sizing
// =====================================================================

size_t PreciseTuner::computeDynamicWindowSize(float detectedFreq) const {
    if (detectedFreq <= 0.0f) return m_windowSize;

    // Need at least 3 complete periods for robust pitch detection
    float periodSamples = static_cast<float>(m_sampleRate) / detectedFreq;
    size_t needed = static_cast<size_t>(std::ceil(periodSamples * 3.0f));

    // Round up to next power of two
    size_t pot = 1;
    while (pot < needed) pot <<= 1;

    // Clamp: min 2048, max 16384
    if (pot < 2048) pot = 2048;
    if (pot > 16384) pot = 16384;

    // For sub-bass (<60Hz), ensure we have enough resolution for ±1 cent
    // At 30 Hz, 48kHz: period = 1600 samples, 3 periods = 4800 → 8192
    if (detectedFreq < 60.0f && pot < 8192) pot = 8192;

    return pot;
}

// =====================================================================
// Note Snapping
// =====================================================================

void PreciseTuner::chromaticSnap(float frequency) {
    if (frequency <= 0.0f) return;

    // MIDI note = 12 * log2(freq / 440) + 69
    float midiFloat = 12.0f * std::log2(frequency / 440.0f) + 69.0f;
    int midiNote = static_cast<int>(std::round(midiFloat));

    // Clamp
    if (midiNote < 0) midiNote = 0;
    if (midiNote > 127) midiNote = 127;

    // Target frequency for this MIDI note
    float targetFreq = 440.0f * std::pow(2.0f, (midiNote - 69) / 12.0f);

    // Cents deviation
    float cents = 1200.0f * std::log2(frequency / targetFreq);

    m_result.targetNoteIndex.store(midiNote, std::memory_order_relaxed);
    m_result.diffCents.store(cents, std::memory_order_relaxed);
    m_result.setNoteName(midiToNoteName(midiNote));
}

void PreciseTuner::presetSnap(float frequency) {
    if (frequency <= 0.0f || m_currentPreset.frequencies.empty()) {
        // Fallback to chromatic
        chromaticSnap(frequency);
        return;
    }

    // Find nearest preset note (using cents distance, not Hz distance)
    int bestIdx = -1;
    float bestCents = 1e6f;

    for (size_t i = 0; i < m_currentPreset.frequencies.size(); ++i) {
        float target = m_currentPreset.frequencies[i];
        if (target <= 0.0f) continue;

        float cents = std::abs(1200.0f * std::log2(frequency / target));
        if (cents < bestCents) {
            bestCents = cents;
            bestIdx = static_cast<int>(i);
        }
    }

    // Reject if deviation > 200 cents (probably a harmonic or wrong string)
    if (bestIdx < 0 || bestCents > 200.0f) {
        m_result.active.store(false, std::memory_order_relaxed);
        return;
    }

    float targetFreq = m_currentPreset.frequencies[bestIdx];
    float cents = 1200.0f * std::log2(frequency / targetFreq);

    m_result.targetNoteIndex.store(bestIdx, std::memory_order_relaxed);
    m_result.diffCents.store(cents, std::memory_order_relaxed);

    // Set note name from preset if available, otherwise from frequency
    if (bestIdx < static_cast<int>(m_currentPreset.noteNames.size())) {
        m_result.setNoteName(m_currentPreset.noteNames[bestIdx].c_str());
    } else {
        // Derive from frequency
        float midiFloat = 12.0f * std::log2(targetFreq / 440.0f) + 69.0f;
        int midiNote = static_cast<int>(std::round(midiFloat));
        m_result.setNoteName(midiToNoteName(midiNote));
    }
}

// =====================================================================
// 4-Pole Butterworth LPF (2 cascaded biquad sections)
// =====================================================================

void PreciseTuner::computeLPFCoefficients(float cutoffHz) {
    if (m_sampleRate == 0) return;

    // Butterworth 4th-order = 2 cascaded 2nd-order sections
    // Pre-warping
    float wc = std::tan(static_cast<float>(M_PI) * cutoffHz / static_cast<float>(m_sampleRate));
    float wc2 = wc * wc;

    // Section 1: Q = 1 / (2 * cos(pi/8)) ≈ 0.5412
    // Section 2: Q = 1 / (2 * cos(3*pi/8)) ≈ 1.3066
    float Q[2] = {
        1.0f / (2.0f * std::cos(static_cast<float>(M_PI) / 8.0f)),
        1.0f / (2.0f * std::cos(3.0f * static_cast<float>(M_PI) / 8.0f))
    };

    for (int i = 0; i < 2; ++i) {
        float alpha = wc / Q[i];
        float denom = 1.0f + alpha + wc2;
        float invDenom = 1.0f / denom;

        m_lpfCoeffs[i].b0 = wc2 * invDenom;
        m_lpfCoeffs[i].b1 = 2.0f * wc2 * invDenom;
        m_lpfCoeffs[i].b2 = wc2 * invDenom;
        m_lpfCoeffs[i].a1 = 2.0f * (wc2 - 1.0f) * invDenom;
        m_lpfCoeffs[i].a2 = (1.0f - alpha + wc2) * invDenom;
    }
}

void PreciseTuner::applyLPF(float* buffer, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float sample = buffer[i];
        for (int section = 0; section < 2; ++section) {
            auto& s = m_lpfState[section];
            auto& c = m_lpfCoeffs[section];
            float y = c.b0 * sample + c.b1 * s.x1 + c.b2 * s.x2
                                    - c.a1 * s.y1 - c.a2 * s.y2;
            s.x2 = s.x1;
            s.x1 = sample;
            s.y2 = s.y1;
            s.y1 = y;
            sample = y;
        }
        buffer[i] = sample;
    }
}

void PreciseTuner::resetLPF() {
    for (int i = 0; i < 2; ++i) {
        m_lpfState[i] = {};
    }
}
