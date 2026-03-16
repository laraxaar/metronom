/*
 * Omnibeat Native Audio Engine v2 -- C++ / miniaudio
 * ====================================================================
 * Sample-accurate metronome, low-latency I/O, device enumeration,
 * onset detection (tremolo-safe with spectral-flux + BPM-context
 * debounce), polyrhythm, groove-shift, tempo-map, tap-tempo BPM
 * detection, offline WAV export, and sample resampling.
 *
 * Everything runs GIL-free on the audio thread except Python API
 * wrappers which only copy data in/out.
 */

#define NOMINMAX
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <deque>
#include <string>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <ctime>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Forward declarations
struct Engine;
static Engine& get_engine();

// =====================================================================
//  Structs
// =====================================================================

struct ActiveSound {
    std::vector<float> data;
    size_t cursor = 0;
};

struct TempoMapEntry {
    double start_sec, end_sec;
    double start_bpm, end_bpm;       // supports smooth ramps
};

struct PolyVoice {
    int    clicks_remaining;
    double interval_frames;
    double next_frame;
    std::vector<float> tone;
};

struct HitEvent {
    double diff_ms;
    char   status[12];    // "Early", "Late", "Perfect"
    char   grade[20];
};

struct ClickEvent {
    int beat;
    int substep;
};

// =====================================================================
//  Free Play BPM Tracker (native C++ for precision)
// =====================================================================
// Tracks inter-onset intervals to compute live BPM during free play.

struct FreePlayTracker {
    std::deque<double> onset_times;     // timestamps in seconds
    int    max_onsets  = 50;            // sliding window
    double window_sec  = 8.0;           // only consider last N seconds
    std::atomic<bool>  active{false};

    // Results (atomic for lock-free reads from Python)
    std::atomic<double> live_bpm{0.0};
    std::atomic<double> stability{100.0};  // 0-100%
    std::atomic<double> min_bpm{9999.0};
    std::atomic<double> max_bpm{0.0};
    std::atomic<double> avg_bpm{0.0};
    std::atomic<int>    drift_direction{0};  // -1 slowing, 0 steady, +1 speeding
    std::atomic<int>    total_hits{0};

    void reset() {
        onset_times.clear();
        live_bpm.store(0.0);
        stability.store(100.0);
        min_bpm.store(9999.0);
        max_bpm.store(0.0);
        avg_bpm.store(0.0);
        drift_direction.store(0);
        total_hits.store(0);
    }

    void record_onset(double ts_sec) {
        onset_times.push_back(ts_sec);
        total_hits.fetch_add(1);

        // Trim old onsets
        while (!onset_times.empty() && (ts_sec - onset_times.front()) > window_sec)
            onset_times.pop_front();
        while ((int)onset_times.size() > max_onsets)
            onset_times.pop_front();

        if (onset_times.size() < 2) return;

        // Compute intervals
        std::vector<double> intervals;
        for (size_t i = 1; i < onset_times.size(); ++i) {
            double gap = onset_times[i] - onset_times[i-1];
            if (gap > 0.1 && gap < 3.0)  // 20-600 BPM range
                intervals.push_back(gap);
        }
        if (intervals.empty()) return;

        // Current BPM from last few intervals (responsive)
        int recent_n = std::min((int)intervals.size(), 4);
        double recent_sum = 0;
        for (int i = (int)intervals.size() - recent_n; i < (int)intervals.size(); ++i)
            recent_sum += intervals[i];
        double recent_avg = recent_sum / recent_n;
        double cur = 60.0 / recent_avg;
        live_bpm.store(cur);

        // Overall average
        double total = std::accumulate(intervals.begin(), intervals.end(), 0.0);
        double mean_ivl = total / intervals.size();
        double mean_bpm = 60.0 / mean_ivl;
        avg_bpm.store(mean_bpm);

        // Min/Max tracking
        if (cur < min_bpm.load() && cur > 15.0) min_bpm.store(cur);
        if (cur > max_bpm.load() && cur < 600.0) max_bpm.store(cur);

        // Stability (coefficient of variation)
        double sq_sum = 0;
        for (auto x : intervals) sq_sum += (x - mean_ivl) * (x - mean_ivl);
        double cv = sqrt(sq_sum / intervals.size()) / std::max(0.001, mean_ivl);
        double stab = std::max(0.0, 100.0 - cv * 300.0);
        stability.store(stab);

        // Drift detection: compare first half vs second half
        if (intervals.size() >= 6) {
            int half = (int)intervals.size() / 2;
            double first_avg = 0, second_avg = 0;
            for (int i = 0; i < half; ++i) first_avg += intervals[i];
            for (int i = half; i < (int)intervals.size(); ++i) second_avg += intervals[i];
            first_avg /= half;
            second_avg /= ((int)intervals.size() - half);
            double diff_pct = (second_avg - first_avg) / first_avg * 100.0;
            if (diff_pct < -3.0) drift_direction.store(1);   // intervals shrinking = speeding up
            else if (diff_pct > 3.0) drift_direction.store(-1);  // intervals growing = slowing down
            else drift_direction.store(0);  // steady
        }
    }
};

// =====================================================================
//  Tap-Tempo BPM Detector (native C++)
// =====================================================================

struct TapDetector {
    std::deque<double> timestamps;
    int    buffer_size = 10;
    double window_sec  = 4.0;

    void tap(double ts) {
        timestamps.push_back(ts);
        // Remove old taps outside window
        while (!timestamps.empty() && (ts - timestamps.front()) > window_sec)
            timestamps.pop_front();
        if ((int)timestamps.size() > buffer_size)
            timestamps.pop_front();
    }

    double get_bpm() const {
        if (timestamps.size() < 2) return 0.0;
        double sum = 0.0;
        for (size_t i = 1; i < timestamps.size(); ++i)
            sum += timestamps[i] - timestamps[i - 1];
        double avg = sum / (timestamps.size() - 1);
        return avg > 0 ? 60.0 / avg : 0.0;
    }

    double get_stability() const {
        if (timestamps.size() < 3) return 100.0;
        std::vector<double> intervals;
        for (size_t i = 1; i < timestamps.size(); ++i)
            intervals.push_back(timestamps[i] - timestamps[i - 1]);
        double mean = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
        double sq_sum = 0.0;
        for (auto x : intervals) sq_sum += (x - mean) * (x - mean);
        double std_dev = sqrt(sq_sum / intervals.size());
        return std::max(0.0, 100.0 - std_dev * 500.0);
    }

    void reset() { timestamps.clear(); }
};

// =====================================================================
//  Smart Onset Detector (tremolo-safe)
// =====================================================================
// Uses spectral flux (energy derivative) + BPM-context debounce +
// attack envelope detection so that tremolo picking / rolls don't
// register as individual beats.

struct SmartOnsetDetector {
    // Parameters
    double base_threshold       = 0.08;
    double debounce_min_sec     = 0.04;   // absolute minimum
    double bpm_debounce_factor  = 0.35;   // fraction of beat interval
    bool   use_spectral_flux    = true;
    double flux_threshold       = 0.02;   // minimum spectral flux rise

    // State
    std::deque<float> energy_history;
    float  prev_energy          = 0.0f;
    double adaptive_thr         = 0.08;
    double last_hit_sec         = 0.0;
    double sustained_start_sec  = 0.0;    // tremolo burst tracking
    int    rapid_hit_count      = 0;
    double prev_flux            = 0.0;

    void reset() {
        energy_history.clear();
        prev_energy = 0.0f;
        adaptive_thr = base_threshold;
        last_hit_sec = 0.0;
        sustained_start_sec = 0.0;
        rapid_hit_count = 0;
        prev_flux = 0.0;
    }

    // Returns true if a genuine beat onset is detected (not tremolo noise)
    bool process_window(const float* samples, int n, uint32_t sr,
                        double current_bpm, double current_sec)
    {
        // 1. RMS energy
        float sum2 = 0.f;
        for (int i = 0; i < n; ++i) sum2 += samples[i] * samples[i];
        float rms = sqrtf(sum2 / std::max(1, n));

        // 2. Spectral flux (energy derivative)
        float flux = rms - prev_energy;
        prev_energy = rms;

        energy_history.push_back(rms);
        if (energy_history.size() > 200) energy_history.pop_front();

        // 3. Adaptive threshold (median * 4 + base)
        if (energy_history.size() > 30) {
            std::vector<float> tmp(energy_history.begin(), energy_history.end());
            std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
            adaptive_thr = tmp[tmp.size() / 2] * 4.0 + base_threshold;
        }

        // 4. Must exceed adaptive threshold
        if (rms <= adaptive_thr) {
            // If energy drops, reset the tremolo burst counter
            if (rms < adaptive_thr * 0.5) {
                rapid_hit_count = 0;
            }
            return false;
        }

        // 5. Spectral flux gate: only trigger on rising edges
        if (use_spectral_flux && flux < flux_threshold) {
            prev_flux = flux;
            return false;
        }
        prev_flux = flux;

        // 6. BPM-context-aware debounce
        //    The minimum time between accepted hits scales with the BPM.
        //    At 120 BPM, interval = 0.5s, debounce = 0.175s
        //    At 60 BPM,  interval = 1.0s, debounce = 0.35s
        //    This prevents tremolo picking (which fires at 15-30+ Hz)
        //    from being interpreted as separate beats.
        double beat_interval = (current_bpm > 0) ? (60.0 / current_bpm) : 0.5;
        double debounce = std::max(debounce_min_sec,
                                    beat_interval * bpm_debounce_factor);

        double gap = current_sec - last_hit_sec;
        if (gap < debounce) {
            // Track rapid hits for tremolo detection
            rapid_hit_count++;
            return false;
        }

        // 7. Tremolo burst suppression:
        //    If we've seen many rapid hits recently, require a much
        //    stronger attack to register as a new beat.
        if (rapid_hit_count > 5) {
            // Require the flux to be 4x the threshold for tremolo breaking
            if (flux < flux_threshold * 4.0) {
                return false;
            }
        }

        // 8. Adaptive Flux Threshold:
        //    If the signal is very noisy/distorted, we increase the flux gate.
        if (adaptive_thr > 0.3) {
             if (flux < flux_threshold * 2.0) return false;
        }

        // Accept as genuine onset
        last_hit_sec = current_sec;
        rapid_hit_count = 0;
        return true;
    }
};

// =====================================================================
//  Tuner (YIN Algorithm)
// =====================================================================

struct Tuner {
    std::vector<float> buffer;
    size_t cursor = 0;
    std::atomic<float> current_hz{0.0f};
    std::atomic<float> confidence{0.0f};  // 0.0-1.0 confidence in pitch estimate
    bool enabled = false;

    // LPF state for Hi-Gain suppression
    float lpf_state = 0.0f;
    float lpf_alpha = 0.15f;  // ~1.2kHz cutoff at 44.1kHz

    // YIN parameters — tuned for maximum accuracy
    float threshold = 0.10f;       // Lower = stricter pitch lock (was 0.15)
    size_t window_size = 8192;     // 8192 samples for bass resolution (E1=41Hz needs ~1073 samples)

    void init(uint32_t sr) {
        // 8192 for 44.1+kHz (covers down to ~21Hz fundamental)
        // 4096 for lower sample rates
        window_size = (sr >= 44100) ? 8192 : 4096;
        buffer.assign(window_size * 2, 0.0f);
        cursor = 0;
        lpf_state = 0.0f;
        // alpha = 2*pi*fc / sr
        lpf_alpha = (2.0f * (float)M_PI * 1000.0f) / (float)sr; 
        if (lpf_alpha > 1.0f) lpf_alpha = 1.0f;
    }

    void process(const float* in, int n, uint32_t sr) {
        if (!enabled) return;

        for (int i = 0; i < n; ++i) {
            // Apply LPF to suppress hi-gain harmonics before YIN
            lpf_state += lpf_alpha * (in[i] - lpf_state);
            
            buffer[cursor] = lpf_state;
            cursor++;

            // When buffer reaches window_size, compute pitch
            if (cursor >= window_size) {
                float conf_out = 0.0f;
                float hz = compute_yin(sr, conf_out);
                if (hz > 20.0f && hz < 2000.0f && conf_out > 0.3f) {
                    // Adaptive smoothing: fast response (0.6/0.4) for quick lock
                    float prev = current_hz.load();
                    if (prev == 0.0f) {
                        current_hz.store(hz);
                    } else {
                        // Weighted by confidence: high confidence = less smoothing
                        float alpha = 0.4f + 0.2f * conf_out;  // 0.4-0.6 new weight
                        current_hz.store(prev * (1.0f - alpha) + hz * alpha);
                    }
                    confidence.store(conf_out);
                } else if (hz == 0.0f) {
                    // Decay confidence when no pitch found
                    float c = confidence.load();
                    confidence.store(c * 0.9f);
                }
                
                // 75% overlap for faster updates (shift by 25% of window)
                size_t shift = window_size / 4;
                std::copy(buffer.begin() + shift, buffer.begin() + cursor, buffer.begin());
                cursor -= shift;
            }
        }
    }

private:
    float compute_yin(uint32_t sr, float& out_confidence) {
        size_t W = window_size / 2;
        std::vector<float> yin(W, 0.0f);

        // 1. Difference function
        for (size_t tau = 1; tau < W; ++tau) {
            for (size_t i = 0; i < W; ++i) {
                float delta = buffer[i] - buffer[i + tau];
                yin[tau] += delta * delta;
            }
        }

        // 2. Cumulative mean normalized difference
        float running_sum = 0.0f;
        yin[0] = 1.0f;
        for (size_t tau = 1; tau < W; ++tau) {
            running_sum += yin[tau];
            yin[tau] = yin[tau] * tau / std::max(1e-6f, running_sum);
        }

        // 3. Absolute threshold — find first dip below threshold
        size_t estimate_tau = 0;
        float best_val = 1.0f;
        for (size_t tau = 2; tau < W; ++tau) {
            if (yin[tau] < threshold) {
                // Walk to local minimum
                while (tau + 1 < W && yin[tau + 1] < yin[tau]) {
                    tau++;
                }
                estimate_tau = tau;
                best_val = yin[tau];
                break;
            }
        }

        // Confidence: 1.0 - yin_value (lower yin = higher confidence)
        out_confidence = (estimate_tau > 0) ? std::max(0.0f, std::min(1.0f, 1.0f - best_val)) : 0.0f;

        // 4. OCTAVE VALIDATION (Hi-Gain harmonic rejection)
        //    When YIN locks onto the 2nd harmonic (often on high-gain guitar),
        //    check if tau*2 (octave below) is also a strong dip. If yes, use it.
        if (estimate_tau > 0 && estimate_tau * 2 < W) {
            size_t octave_tau = estimate_tau * 2;
            // Search around octave_tau for a local minimum
            size_t best_oct = octave_tau;
            float best_oct_val = yin[octave_tau];
            for (size_t dt = 1; dt <= 3 && octave_tau + dt < W; ++dt) {
                if (yin[octave_tau + dt] < best_oct_val) {
                    best_oct_val = yin[octave_tau + dt];
                    best_oct = octave_tau + dt;
                }
                if (octave_tau >= dt && yin[octave_tau - dt] < best_oct_val) {
                    best_oct_val = yin[octave_tau - dt];
                    best_oct = octave_tau - dt;
                }
            }
            // If octave-below dip is reasonably strong (within 2x of original),
            // prefer it — this means the original was a harmonic, not fundamental
            if (best_oct_val < threshold * 2.0f && best_oct_val < best_val * 2.5f) {
                estimate_tau = best_oct;
                best_val = best_oct_val;
                out_confidence = std::max(0.0f, std::min(1.0f, 1.0f - best_val));
            }
        }

        // 5. Parabolic interpolation for sub-sample accuracy
        if (estimate_tau > 0 && estimate_tau < W - 1) {
            float s0 = yin[estimate_tau - 1];
            float s1 = yin[estimate_tau];
            float s2 = yin[estimate_tau + 1];
            float denom = 2.0f * s1 - s2 - s0;
            if (fabsf(denom) > 1e-6f) {
                float shift = 0.5f * (s2 - s0) / denom;
                return (float)sr / (estimate_tau + shift);
            }
            return (float)sr / estimate_tau;
        }

        return 0.0f; // No pitch found
    }
};

// =====================================================================
//  Tempo Coaching Engine
// =====================================================================
// Analyzes hit accuracy to advise: "increase tempo", "keep tempo", "decrease tempo"

struct TempoCoach {
    double target_bpm       = 0;
    int    total_hits       = 0;
    int    perfect_hits     = 0;   // <10ms deviation
    int    good_hits        = 0;   // <25ms
    int    ok_hits          = 0;   // <40ms
    int    miss_hits        = 0;   // >=40ms
    double sum_deviation    = 0;
    double avg_deviation_ms = 0;
    std::atomic<int> advice_level{1};  // 0=can increase, 1=keep, 2=decrease

    // Rolling window of recent deviations for trend detection
    std::deque<double> recent_devs;
    int window_size = 32;

    void reset(double bpm) {
        target_bpm = bpm;
        total_hits = perfect_hits = good_hits = ok_hits = miss_hits = 0;
        sum_deviation = avg_deviation_ms = 0;
        advice_level.store(1);
        recent_devs.clear();
    }

    void record_hit(double deviation_ms) {
        double a = fabs(deviation_ms);
        total_hits++;
        sum_deviation += a;
        avg_deviation_ms = sum_deviation / total_hits;

        if      (a < 10)  perfect_hits++;
        else if (a < 25)  good_hits++;
        else if (a < 40)  ok_hits++;
        else              miss_hits++;

        recent_devs.push_back(a);
        while ((int)recent_devs.size() > window_size)
            recent_devs.pop_front();

        compute_advice();
    }

    void compute_advice() {
        if (total_hits < 8) { advice_level.store(1); return; }

        double perfect_pct = (double)perfect_hits / total_hits * 100.0;
        double miss_pct    = (double)miss_hits / total_hits * 100.0;

        // Can increase tempo: >60% perfect, <5% misses
        if (perfect_pct > 60.0 && miss_pct < 5.0 && avg_deviation_ms < 15.0) {
            advice_level.store(0);
        }
        // Should decrease tempo: >30% misses or avg deviation > 45ms
        else if (miss_pct > 30.0 || avg_deviation_ms > 45.0) {
            advice_level.store(2);
        }
        // Keep current tempo
        else {
            advice_level.store(1);
        }
    }
};

struct BPMLadder {
    std::atomic<bool> enabled{false};
    std::atomic<int>  increment{5};
    std::atomic<int>  measures_per_step{4};
    std::atomic<int>  target_bpm{200};
    std::atomic<int>  measure_count{0};
    
    void* engine_ptr = nullptr;

    void reset() { measure_count.store(0); }
    
    void on_measure(std::atomic<double>& bpm);
};

struct DisappearingMetronome {
    std::atomic<bool> enabled{false};
    std::atomic<int>  visible_measures{4};
    std::atomic<int>  invisible_measures{4};
    std::atomic<int>  measure_count{0};
    std::atomic<bool> is_silent{false};

    void reset() { measure_count.store(0); is_silent.store(false); }

    void on_measure() {
        if (!enabled.load()) { is_silent.store(false); return; }
        int cur_m = measure_count.fetch_add(1) + 1;
        if (!is_silent.load() && cur_m >= visible_measures.load()) {
            is_silent.store(true);
            measure_count.store(0);
        } else if (is_silent.load() && cur_m >= invisible_measures.load()) {
            is_silent.store(false);
            measure_count.store(0);
        }
    }
};

struct RandomSilence {
    std::atomic<bool> enabled{false};
    std::atomic<float> probability{0.2f};
    std::atomic<bool> is_silent{false};

    void on_tick() {
        if (!enabled.load()) { is_silent.store(false); return; }
        float r = (float)rand() / (float)RAND_MAX;
        is_silent.store(r < probability.load());
    }
};

struct OffbeatTrainer {
    std::atomic<bool> enabled{false};
    std::atomic<bool> is_silent{false};

    void on_tick(int beat, int substep) {
        if (!enabled.load()) { is_silent.store(false); return; }
        is_silent.store(beat % 2 == 0 && substep == 0);
    }
};

struct RhythmBoss {
    std::atomic<bool> enabled{false};
    std::atomic<int>  level{1};
    std::atomic<int>  tick_count{0};
    std::atomic<bool> is_silent{false};

    void reset() { tick_count.store(0); is_silent.store(false); }

    void on_tick(std::atomic<double>& bpm) {
        if (!enabled.load()) { is_silent.store(false); return; }
        int cur_t = tick_count.fetch_add(1) + 1;
        int lvl = level.load();
        
        // Random silence
        float r = (float)rand() / (float)RAND_MAX;
        is_silent.store(r < 0.03f * lvl);

        // Tempo jitter
        if (lvl >= 3 && cur_t % 16 == 0) {
            int jitter = (rand() % (5 * lvl * 2 + 1)) - (5 * lvl);
            double next = bpm.load() + (double)jitter;
            if (next < 40.0) next = 40.0;
            if (next > 600.0) next = 600.0;
            bpm.store(next);
        }
    }
};

struct HumanMetronome {
    std::atomic<bool> enabled{false};
    std::atomic<int>  play_bars{4};
    std::atomic<int>  test_bars{4};
    std::atomic<int>  measure_count{0};
    std::atomic<bool> is_silent{false};

    void reset() { measure_count.store(0); is_silent.store(false); }

    void on_measure() {
        if (!enabled.load()) { is_silent.store(false); return; }
        int cur_m = measure_count.fetch_add(1) + 1;
        if (!is_silent.load() && cur_m >= play_bars.load()) {
            is_silent.store(true);
            measure_count.store(0);
        } else if (is_silent.load() && cur_m >= test_bars.load()) {
            is_silent.store(false);
            measure_count.store(0);
        }
    }
};

// =====================================================================
//  Global Engine State
// =====================================================================

struct Engine {
    ma_context* ctx{nullptr};
    bool        ctx_inited{false};
    ma_device*  device{nullptr};
    bool        dev_inited{false};
    std::atomic<bool> running{false};

    uint32_t sample_rate = 44100;

    // -- Metronome state --
    std::atomic<double> bpm{120.0};
    std::atomic<int>    subdivision{1};
    std::atomic<double> scale{1.0};
    std::atomic<int>    time_sig_top{4};

    std::atomic<int>      current_beat{0};
    std::atomic<int>      current_substep{0};
    std::atomic<uint64_t> total_frames{0};
    std::atomic<double>   next_click_frame_d{0.0};
    std::atomic<uint64_t> next_click_frame{0};

    // -- Click samples --
    std::vector<float> accent_sample;
    std::vector<float> normal_sample;

    // -- Active sounds --
    std::vector<ActiveSound> active_sounds;
    CRITICAL_SECTION mix_mtx;

    // -- Mute / groove shift --
    std::atomic<bool>    muted{false};
    std::atomic<int64_t> groove_shift_frames{0};

    // -- Polyrhythm --
    std::vector<PolyVoice> poly_voices;
    CRITICAL_SECTION poly_mtx;

    // -- Tempo map --
    std::vector<TempoMapEntry> tempo_map;
    std::atomic<bool> tempo_map_active{false};
    CRITICAL_SECTION tmap_mtx;

    // -- Input analysis --
    std::atomic<bool>   analyzing{false};
    SmartOnsetDetector  onset;

    // Results queue
    std::vector<HitEvent> pending_hits;
    CRITICAL_SECTION hit_mtx;

    // -- Beat notification --
    std::atomic<int> last_beat{-1};
    std::atomic<int> last_substep{-1};

    // -- Tap tempo --
    TapDetector tap_det;
    CRITICAL_SECTION tap_mtx;

    // -- Input peak meter --
    std::atomic<float> input_peak{0.0f};
    std::atomic<float> output_peak{0.0f};
    std::atomic<bool>  monitoring{false};
    std::atomic<float> monitor_gain{1.0f};

    // -- Training Modules --
    BPMLadder             ladder;
    DisappearingMetronome disappearing;
    RandomSilence         random_silence;
    OffbeatTrainer        offbeat;
    RhythmBoss            boss;
    HumanMetronome        human;

    // -- Debug/Logging --
    std::deque<std::string> log_queue;
    CRITICAL_SECTION        log_mtx;

    void log(const std::string& msg) {
        EnterCriticalSection(&log_mtx);
        if (log_queue.size() > 100) log_queue.pop_front();
        log_queue.push_back(msg);
        LeaveCriticalSection(&log_mtx);
    }

    // -- Tuner --
    Tuner tuner;

    // -- Free Play Tracker --
    FreePlayTracker free_play;

    // -- Tempo Coach --
    TempoCoach tempo_coach;
    CRITICAL_SECTION coach_mtx;

    // -- Drum mode onset parameters --
    std::atomic<bool> drum_mode{false};

    // -- Click count (for UI beat visualizer) --
    std::atomic<uint64_t> click_count{0};

    // -- Click Queue (Accuracy Fix for Training Modules) --
    std::deque<ClickEvent> click_queue;
    CRITICAL_SECTION       click_mtx;

    // -- Per-beat Patterns (Accent/Mute/Normal) --
    std::vector<int>       beat_pattern; // 0=Mute, 1=Normal, 2=Accent
    CRITICAL_SECTION       pattern_mtx;

    Engine() {
        ctx = new ma_context();
        device = new ma_device();
        
        srand((unsigned int)time(NULL));

        InitializeCriticalSection(&mix_mtx);
        InitializeCriticalSection(&poly_mtx);
        InitializeCriticalSection(&tmap_mtx);
        InitializeCriticalSection(&hit_mtx);
        InitializeCriticalSection(&tap_mtx);
        InitializeCriticalSection(&click_mtx);
        InitializeCriticalSection(&pattern_mtx);
        InitializeCriticalSection(&coach_mtx);
        InitializeCriticalSection(&log_mtx);

        // Pass pointer to sub-structs that need logging
        ladder.engine_ptr = this;
    }

    ~Engine() {
        // Uninitialize miniaudio resources if they were initialized
        if (dev_inited) { ma_device_uninit(device); }
        if (ctx_inited) { ma_context_uninit(ctx); }

        // Delete dynamically allocated miniaudio objects
        if (device) { delete device; device = nullptr; }
        if (ctx) { delete ctx; ctx = nullptr; }

        DeleteCriticalSection(&mix_mtx);
        DeleteCriticalSection(&poly_mtx);
        DeleteCriticalSection(&tmap_mtx);
        DeleteCriticalSection(&hit_mtx);
        DeleteCriticalSection(&tap_mtx);
        DeleteCriticalSection(&click_mtx);
        DeleteCriticalSection(&pattern_mtx);
        DeleteCriticalSection(&coach_mtx);
        DeleteCriticalSection(&log_mtx);
    }
};

static Engine* g_ptr = nullptr;
static Engine& get_engine() {
    if (!g_ptr) g_ptr = new Engine();
    return *g_ptr;
}
#define g (get_engine())

// -- BPMLadder Implementation --
void BPMLadder::on_measure(std::atomic<double>& bpm) {
    if (!enabled.load()) return;
    int cur_m = measure_count.fetch_add(1) + 1;
    if (cur_m >= measures_per_step.load()) {
        measure_count.store(0);
        double cur = bpm.load();
        double next = cur + (double)increment.load();
        if (next <= (double)target_bpm.load()) {
            bpm.store(next);
            if (engine_ptr) {
                ((Engine*)engine_ptr)->log("BPM Ladder: Increased BPM to " + std::to_string(next));
            }
        }
    }
}

class CSLock {
    CRITICAL_SECTION* cs;
public:
    CSLock(CRITICAL_SECTION& c) : cs(&c) { EnterCriticalSection(cs); }
    ~CSLock() { LeaveCriticalSection(cs); }
};

// =====================================================================
//  Helpers
// =====================================================================

static double frames_per_interval(uint32_t sr) {
    double b = g.bpm.load();
    double s = g.scale.load();
    int    d = g.subdivision.load();
    if (b <= 0 || s <= 0 || d <= 0) return sr;
    return (60.0 / (b * s * d)) * sr;
}

static void enqueue_sound(const std::vector<float>& src) {
    ActiveSound snd;
    snd.data = src;
    snd.cursor = 0;
    g.active_sounds.push_back(std::move(snd));
}

static std::vector<float> make_tone(float freq, float dur, float amp, uint32_t sr) {
    int n = (int)(sr * dur);
    std::vector<float> buf(n);
    for (int i = 0; i < n; ++i)
        buf[i] = amp * sinf(2.f * (float)M_PI * freq * i / sr);
    int fade = (int)(n * 0.15f);
    for (int i = 0; i < fade; ++i)
        buf[n - fade + i] *= (float)(fade - i) / fade;
    return buf;
}

// Resample using linear interpolation (C++)
static std::vector<float> resample(const float* data, size_t len,
                                    uint32_t src_sr, uint32_t dst_sr)
{
    if (src_sr == dst_sr || len == 0) {
        return std::vector<float>(data, data + len);
    }
    double ratio = (double)dst_sr / src_sr;
    size_t out_len = (size_t)(len * ratio);
    std::vector<float> out(out_len);
    for (size_t i = 0; i < out_len; ++i) {
        double src_idx = i / ratio;
        size_t idx0 = (size_t)src_idx;
        size_t idx1 = std::min(idx0 + 1, len - 1);
        double frac = src_idx - idx0;
        out[i] = (float)((1.0 - frac) * data[idx0] + frac * data[idx1]);
    }
    return out;
}

// =====================================================================
//  Audio Callback
// =====================================================================

static void audio_callback(ma_device* pDev, void* pOut, const void* pIn,
                            ma_uint32 frameCount)
{

    try {
        float*       out = (float*)pOut;
        const float* in  = (const float*)pIn;
        uint32_t     sr  = pDev->sampleRate;
        uint32_t     och = pDev->playback.channels;

        // -- 1. INPUT ANALYSIS (smart onset detection) -------------------
        if (g.analyzing.load() && in) {
        int win = std::max(1, (int)(0.01 * sr));  // ~10ms windows
        float block_peak = 0.f;

        for (ma_uint32 i = 0; i < frameCount; i += win) {
            int n = std::min((int)(frameCount - i), win);
            double cur_sec = (double)(g.total_frames.load() + i) / sr;
            double cur_bpm = g.bpm.load() * g.scale.load();

            // Track input peak for meter
            for (int j = 0; j < n; ++j) {
                float absv = fabsf(in[i + j]);
                if (absv > block_peak) block_peak = absv;
            }

            bool onset = g.onset.process_window(
                in + i, n, sr, cur_bpm, cur_sec
            );

            // Free Play: record onset for live BPM tracking
            if (onset && g.free_play.active.load()) {
                g.free_play.record_onset(cur_sec);
            }

            if (onset) {
                // Determine reference BPM for error calculation
                double ref_bpm = 0.0;
                if (g.free_play.active.load()) {
                    ref_bpm = g.free_play.avg_bpm.load(); // Compare against your own established tempo
                } else {
                    ref_bpm = g.bpm.load() * g.scale.load(); // Compare against metronome/tempo map
                }

                // Calculate distance to nearest beat
                double fpb = (ref_bpm > 0) ? (60.0 / ref_bpm) * sr : 0;
                if (fpb > 0) {
                    double since = fmod((double)(g.total_frames + i), fpb);
                    double to_next = fpb - since;

                    HitEvent ev;
                    memset(&ev, 0, sizeof(ev));

                    if (since < to_next) {
                        ev.diff_ms = (since / sr) * 1000.0;
                        strncpy(ev.status, "Late", sizeof(ev.status) - 1);
                    } else {
                        ev.diff_ms = -(to_next / sr) * 1000.0;
                        strncpy(ev.status, "Early", sizeof(ev.status) - 1);
                    }

                    double a = fabs(ev.diff_ms);
                    if      (a < 10)  strncpy(ev.grade, "Perfect S+", sizeof(ev.grade) - 1);
                    else if (a < 25)  strncpy(ev.grade, "Great S",    sizeof(ev.grade) - 1);
                    else if (a < 40)  strncpy(ev.grade, "Good A",     sizeof(ev.grade) - 1);
                    else if (a < 60)  strncpy(ev.grade, "OK B",       sizeof(ev.grade) - 1);
                    else              strncpy(ev.grade, "Miss C",     sizeof(ev.grade) - 1);

                    EnterCriticalSection(&g.hit_mtx);
                    g.pending_hits.push_back(ev);
                    LeaveCriticalSection(&g.hit_mtx);

                    // Feed tempo coach
                    {
                        CSLock lk(g.coach_mtx);
                        g.tempo_coach.record_hit(ev.diff_ms);
                    }
                }
            }
        }

        // Update peak meter
        float old_peak = g.input_peak.load();
        if (block_peak > old_peak) {
            g.input_peak.store(block_peak);
        } else {
            g.input_peak.store(old_peak * 0.95f);
        }

        // Let the Tuner analyze the input
        g.tuner.process(in, frameCount, sr);
    } // <-- Close if (g.analyzing.load() && in)

    // -- 2. OUTPUT ---------------------------------------------------
    float block_output_peak = 0.f;
    bool  is_monitoring = g.monitoring.load();
    float mon_gain    = g.monitor_gain.load();

    for (ma_uint32 i = 0; i < frameCount; ++i) {

        // Click trigger
        if (g.total_frames.load() >= g.next_click_frame.load()) {
            int64_t shift = g.groove_shift_frames.exchange(0);
            
            // 1. Update Tempo map (with smooth ramps) inside the trigger block
            // This ensures BPM is correct for the exact moment of the click
            if (g.tempo_map_active.load()) {
                double elapsed = (double)g.total_frames.load() / sr;
                CSLock lk(g.tmap_mtx);
                for (auto& e : g.tempo_map) {
                    if (elapsed >= e.start_sec && elapsed < e.end_sec) {
                        if (e.start_bpm == e.end_bpm) {
                            g.bpm.store(e.start_bpm);
                        } else {
                            // Linear ramp between start_bpm and end_bpm
                            double progress = (elapsed - e.start_sec) /
                                              (e.end_sec - e.start_sec);
                            double ramped = e.start_bpm +
                                            (e.end_bpm - e.start_bpm) * progress;
                            g.bpm.store(ramped);
                        }
                        break;
                    }
                }
            }
            
            // Per-beat role determination (0:Mute, 1:Normal, 2:Accent)
            int role = 1; // Default to Normal
            {
                CSLock lk(g.pattern_mtx);
                if (!g.beat_pattern.empty()) {
                    int idx = g.current_beat % (int)g.beat_pattern.size();
                    role = g.beat_pattern[idx];
                } else {
                    // Fallback: Beat 0 is accent
                    role = (g.current_beat == 0 && g.current_substep == 0) ? 2 : 1;
                }
            }
            
            // Training modules: process on every measure (beat 0, substep 0)
            if (g.current_beat.load() == 0 && g.current_substep.load() == 0) {
                g.ladder.on_measure(g.bpm);
                g.disappearing.on_measure();
                g.human.on_measure();
            }

            // Per-tick modules (only if it's the start of a beat/substep)
            if (g.current_substep.load() == 0) {
                g.random_silence.on_tick();
                g.offbeat.on_tick(g.current_beat.load(), g.current_substep.load());
                g.boss.on_tick(g.bpm);
            }

            // Final mute check (if engine-wide muted or if any training module is muting)
            bool total_muted = g.muted.load() || 
                               g.disappearing.is_silent.load() || 
                               g.human.is_silent.load() ||
                               g.random_silence.is_silent.load() ||
                               g.offbeat.is_silent.load() ||
                               g.boss.is_silent.load() ||
                               (role == 0 && g.current_substep == 0);
            bool is_accent   = (role == 2);

            if (!total_muted) {
                CSLock lk(g.mix_mtx);
                const auto& smp = is_accent ? g.accent_sample : g.normal_sample;
                if (!smp.empty()) {
                    ActiveSound snd;
                    snd.data = smp;
                    snd.cursor = 0;
                    g.active_sounds.push_back(std::move(snd));
                }
            }

            g.last_beat.store(g.current_beat);
            g.last_substep.store(g.current_substep);
            g.click_count.fetch_add(1);

            // Push to click queue for absolute accuracy in plugins
            {
                EnterCriticalSection(&g.click_mtx);
                if (g.click_queue.size() < 256) { // Safety cap: 256 entries max (~3.5s at 600BPM×7sub)
                    g.click_queue.push_back({g.current_beat.load(), g.current_substep.load()});
                }
                LeaveCriticalSection(&g.click_mtx);
            }

            double ivl = frames_per_interval(sr);
            // PRECISION: accumulate in double, convert to int only for comparison
            double next_d = g.next_click_frame_d.load() + ivl + (double)shift;
            g.next_click_frame_d.store(next_d);
            g.next_click_frame.store((uint64_t)next_d);

            int next_sub = g.current_substep.load() + 1;
            int top = g.time_sig_top.load();
            int sub = g.subdivision.load();
            if (next_sub >= sub) {
                g.current_substep.store(0);
                int next_beat = (g.current_beat.load() + 1) % top;
                g.current_beat.store(next_beat);
            } else {
                g.current_substep.store(next_sub);
            }
        }

        // Polyrhythm voices
        {
            CSLock lk(g.poly_mtx);
            for (auto it = g.poly_voices.begin(); it != g.poly_voices.end(); ) {
                if ((double)g.total_frames.load() >= it->next_frame && it->clicks_remaining > 0) {
                    if (!it->tone.empty()) enqueue_sound(it->tone);
                    it->clicks_remaining--;
                    it->next_frame += it->interval_frames;
                }
                if (it->clicks_remaining <= 0)
                    it = g.poly_voices.erase(it);
                else
                    ++it;
            }
        }

        // Mix
        float mix = 0.f;
        {
            CSLock lk(g.mix_mtx);
            for (auto it = g.active_sounds.begin(); it != g.active_sounds.end(); ) {
                if (it->cursor < it->data.size()) {
                    mix += it->data[it->cursor++];
                    ++it;
                } else {
                    it = g.active_sounds.erase(it);
                }
            }
        }
        
        // Monitoring: mix in the input signal
        if (is_monitoring && in) {
            mix += in[i] * mon_gain;
        }

        mix = std::max(-1.f, std::min(1.f, mix));
        
        // Output peak calculation
        float abs_mix = fabsf(mix);
        if (abs_mix > block_output_peak) block_output_peak = abs_mix;

        if (out) {
            for (uint32_t c = 0; c < och; ++c)
                out[i * och + c] = mix;
        }
        g.total_frames.fetch_add(1);
    }

    // Update output peak meter
    float old_out_peak = g.output_peak.load();
    if (block_output_peak > old_out_peak) {
        g.output_peak.store(block_output_peak);
    } else {
        g.output_peak.store(old_out_peak * 0.95f);
    }
    } catch (...) {}
}

// =====================================================================
//  Python API
// =====================================================================

// -- init(sample_rate) ------------------------------------------------
static PyObject* py_init(PyObject*, PyObject* args) {
    int sr = 44100;
    if (!PyArg_ParseTuple(args, "|i", &sr)) return NULL;
    g.sample_rate = (uint32_t)sr;

    ma_result result;

    if (!g.ctx_inited) {
        result = ma_context_init(NULL, 0, NULL, g.ctx);
        if (result != MA_SUCCESS) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize audio context");
            return NULL;
        }
        g.ctx_inited = true;
    }

    g.tuner.init(g.sample_rate);

    if (g.dev_inited) {
        ma_device_uninit(g.device);
        g.dev_inited = false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    // Allow user to set custom devices via indices. -1 means default.
    cfg.playback.pDeviceID = NULL; 
    
    cfg.capture.format    = ma_format_f32;
    cfg.capture.channels  = 1;
    cfg.capture.pDeviceID = NULL;

    cfg.sampleRate        = g.sample_rate;
    cfg.dataCallback      = audio_callback;
    cfg.periodSizeInFrames = 128;

    result = ma_device_init(g.ctx, &cfg, g.device);
    
    // Fallback: If duplex fails (often because of missing/locked microphone on Windows),
    // try initializing playback only so the metronome still works.
    if (result != MA_SUCCESS) {
        ma_device_config fallback_cfg = ma_device_config_init(ma_device_type_playback);
        fallback_cfg.playback.format   = ma_format_f32;
        fallback_cfg.playback.channels = 2;
        fallback_cfg.sampleRate        = g.sample_rate;
        fallback_cfg.dataCallback      = audio_callback;
        fallback_cfg.periodSizeInFrames = 128;
        
        if (ma_device_init(g.ctx, &fallback_cfg, g.device) != MA_SUCCESS) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to init miniaudio device (even in fallback playback mode)");
            return NULL;
        }
    }
    
    g.dev_inited = true;
    Py_RETURN_NONE;
}

// -- start / stop / cleanup ------------------------------------------
static PyObject* py_start(PyObject*, PyObject*) {
    if (!g_ptr) return NULL;
    if (!g.dev_inited) {
        PyErr_SetString(PyExc_RuntimeError, "Engine not initialised");
        return NULL;
    }
    g.total_frames.store(0);
    g.next_click_frame.store(0);
    g.next_click_frame_d.store(0.0);
    g.current_beat.store(0);
    g.current_substep.store(0);
    g.onset.reset();
    g.input_peak.store(0.f);
    g.tuner.current_hz.store(0.f);
    g.tuner.confidence.store(0.f);
    g.click_count.store(0);
    { CSLock lk(g.click_mtx); g.click_queue.clear(); }
    { CSLock lk(g.coach_mtx); g.tempo_coach.reset(g.bpm.load()); }
    
    // Reset training modules for a clean start
    g.ladder.reset();
    g.disappearing.reset();
    g.boss.reset();
    g.human.reset();

    { CSLock lk(g.mix_mtx); g.active_sounds.clear(); }
    { CSLock lk(g.poly_mtx); g.poly_voices.clear(); }

    if (ma_device_start(g.device) != MA_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to start audio device");
        return NULL;
    }
    g.running.store(true);
    Py_RETURN_NONE;
}

static PyObject* py_stop(PyObject*, PyObject*) {
    if (!g_ptr) return NULL;
    if (g.dev_inited) ma_device_stop(g.device);
    g.running.store(false);
    { CSLock lk(g.mix_mtx); g.active_sounds.clear(); }
    Py_RETURN_NONE;
}

static PyObject* py_cleanup(PyObject*, PyObject*) {
    if (!g_ptr) return NULL;
    py_stop(NULL, NULL);
    if (g.dev_inited) {
        ma_device_uninit(g.device);
        g.dev_inited = false;
    }
    if (g.ctx_inited) {
        ma_context_uninit(g.ctx);
        g.ctx_inited = false;
    }
    // Delete dynamically allocated miniaudio objects
    if (g.device) { delete g.device; g.device = nullptr; }
    if (g.ctx) { delete g.ctx; g.ctx = nullptr; }
    Py_RETURN_NONE;
}

// -- Metronome setters ------------------------------------------------
static PyObject* py_set_bpm(PyObject*, PyObject* args) {
    double b; if (!PyArg_ParseTuple(args, "d", &b)) return NULL;
    g.bpm.store(std::max(1.0, std::min(9999.0, b)));
    Py_RETURN_NONE;
}
static PyObject* py_set_scale(PyObject*, PyObject* args) {
    double s; if (!PyArg_ParseTuple(args, "d", &s)) return NULL;
    g.scale.store(std::max(0.1, s));
    Py_RETURN_NONE;
}
static PyObject* py_set_subdivision(PyObject*, PyObject* args) {
    int n; if (!PyArg_ParseTuple(args, "i", &n)) return NULL;
    g.subdivision.store(std::max(1, n));
    Py_RETURN_NONE;
}
static PyObject* py_set_time_sig(PyObject*, PyObject* args) {
    int top; if (!PyArg_ParseTuple(args, "i", &top)) return NULL;
    g.time_sig_top.store(std::max(1, top));
    Py_RETURN_NONE;
}
static PyObject* py_set_muted(PyObject*, PyObject* args) {
    int m; if (!PyArg_ParseTuple(args, "p", &m)) return NULL;
    g.muted.store(m != 0);
    Py_RETURN_NONE;
}
static PyObject* py_set_groove_shift(PyObject*, PyObject* args) {
    double ms; if (!PyArg_ParseTuple(args, "d", &ms)) return NULL;
    g.groove_shift_frames.store((int64_t)(ms / 1000.0 * g.sample_rate));
    Py_RETURN_NONE;
}

// -- Samples ----------------------------------------------------------
static PyObject* py_set_samples(PyObject*, PyObject* args) {
    PyObject *a_obj, *n_obj;
    if (!PyArg_ParseTuple(args, "OO", &a_obj, &n_obj)) return NULL;
    Py_buffer va, vn;
    if (PyObject_GetBuffer(a_obj, &va, PyBUF_SIMPLE) != 0) return NULL;
    if (PyObject_GetBuffer(n_obj, &vn, PyBUF_SIMPLE) != 0) {
        PyBuffer_Release(&va); return NULL;
    }
    {
        try {
            float* pa = (float*)va.buf;
            int count_a = va.len / sizeof(float);
            std::vector<float> temp_a; 
            if (pa != nullptr && count_a > 0) {
                temp_a.reserve(count_a);
                for (int i = 0; i < count_a; ++i) temp_a.push_back(pa[i]);
                g.accent_sample = std::move(temp_a);
            }
            
            float* pn = (float*)vn.buf;
            int count_n = vn.len / sizeof(float);
            std::vector<float> temp_n;
            if (pn != nullptr && count_n > 0) {
                temp_n.reserve(count_n);
                for (int i = 0; i < count_n; ++i) temp_n.push_back(pn[i]);
                g.normal_sample = std::move(temp_n);
            }
        } catch (const std::exception& e) {
            PyBuffer_Release(&va);
            PyBuffer_Release(&vn);
            PyErr_SetString(PyExc_RuntimeError, e.what());
            return NULL;
        } catch (...) {
            PyBuffer_Release(&va);
            PyBuffer_Release(&vn);
            PyErr_SetString(PyExc_RuntimeError, "Unknown C++ exception during vector copy");
            return NULL;
        }
    }
    PyBuffer_Release(&va);
    PyBuffer_Release(&vn);
    Py_RETURN_NONE;
}

static PyObject* py_play_sound(PyObject*, PyObject* args) {
    PyObject* obj;
    if (!PyArg_ParseTuple(args, "O", &obj)) return NULL;
    Py_buffer v;
    if (PyObject_GetBuffer(obj, &v, PyBUF_SIMPLE) != 0) return NULL;
    ActiveSound s;
    float* p = (float*)v.buf;
    s.data.assign(p, p + v.len / sizeof(float));
    s.cursor = 0;
    { CSLock lk(g.mix_mtx); g.active_sounds.push_back(std::move(s)); }
    PyBuffer_Release(&v);
    Py_RETURN_NONE;
}

// -- Analysis ---------------------------------------------------------
static PyObject* py_set_analysis(PyObject*, PyObject* args) {
    int e; if (!PyArg_ParseTuple(args, "p", &e)) return NULL;
    g.analyzing.store(e != 0);
    if (e) g.onset.reset();
    Py_RETURN_NONE;
}
static PyObject* py_set_threshold(PyObject*, PyObject* args) {
    double t; if (!PyArg_ParseTuple(args, "d", &t)) return NULL;
    g.onset.base_threshold = t;
    Py_RETURN_NONE;
}

// -- Configure onset detection parameters --
static PyObject* py_set_onset_params(PyObject*, PyObject* args) {
    double debounce_min, bpm_factor, flux_thr;
    int use_flux;
    if (!PyArg_ParseTuple(args, "ddpd", &debounce_min, &bpm_factor,
                          &use_flux, &flux_thr)) return NULL;
    g.onset.debounce_min_sec    = debounce_min;
    g.onset.bpm_debounce_factor = bpm_factor;
    g.onset.use_spectral_flux   = (use_flux != 0);
    g.onset.flux_threshold      = flux_thr;
    Py_RETURN_NONE;
}

static PyObject* py_poll_hits(PyObject*, PyObject*) {
    CSLock lk(g.hit_mtx);
    PyObject* lst = PyList_New(g.pending_hits.size());
    for (size_t i = 0; i < g.pending_hits.size(); ++i) {
        auto& h = g.pending_hits[i];
        PyObject* d = PyDict_New();
        PyDict_SetItemString(d, "diff_ms", PyFloat_FromDouble(h.diff_ms));
        PyDict_SetItemString(d, "status",  PyUnicode_FromString(h.status));
        PyDict_SetItemString(d, "grade",   PyUnicode_FromString(h.grade));
        PyList_SetItem(lst, i, d);
    }
    g.pending_hits.clear();
    return lst;
}

// -- Beat position / state --------------------------------------------
static PyObject* py_get_beat_pos(PyObject*, PyObject*) {
    return Py_BuildValue("(ii)", g.last_beat.load(), g.last_substep.load());
}
static PyObject* py_get_elapsed(PyObject*, PyObject*) {
    return PyFloat_FromDouble((double)g.total_frames.load() / g.sample_rate);
}
static PyObject* py_is_running(PyObject*, PyObject*) {
    return PyBool_FromLong(g.running.load() ? 1 : 0);
}
static PyObject* py_get_bpm(PyObject*, PyObject*) {
    return PyFloat_FromDouble(g.bpm.load());
}
static PyObject* py_get_click_count(PyObject*, PyObject*) {
    return PyLong_FromUnsignedLongLong(g.click_count.load());
}
static PyObject* py_get_input_peak(PyObject*, PyObject*) {
    return PyFloat_FromDouble(g.input_peak.load());
}

static PyObject* py_get_output_peak(PyObject*, PyObject*) {
    return PyFloat_FromDouble(g.output_peak.load());
}

static PyObject* py_set_monitoring(PyObject*, PyObject* args) {
    int active;
    float gain = 1.0f;
    if (!PyArg_ParseTuple(args, "p|f", &active, &gain)) return NULL;
    g.monitoring.store(active != 0);
    g.monitor_gain.store(gain);
    Py_RETURN_NONE;
}

// -- Polyrhythm -------------------------------------------------------
static PyObject* py_add_poly(PyObject*, PyObject* args) {
    int ratio; double mbpm; int bpm_top;
    if (!PyArg_ParseTuple(args, "idi", &ratio, &mbpm, &bpm_top)) return NULL;
    double measure_dur = (60.0 / mbpm) * bpm_top;
    double interval_s  = measure_dur / ratio;
    PolyVoice v;
    v.clicks_remaining = ratio;
    v.interval_frames  = interval_s * (double)g.sample_rate;
    v.next_frame       = (double)g.total_frames;
    v.tone = make_tone(1500.f, 0.018f, 0.3f, g.sample_rate);
    { CSLock lk(g.poly_mtx); g.poly_voices.push_back(std::move(v)); }
    Py_RETURN_NONE;
}

// -- Tempo map --------------------------------------------------------
static PyObject* py_set_tempo_map(PyObject*, PyObject* args) {
    PyObject* lst;
    if (!PyArg_ParseTuple(args, "O", &lst)) return NULL;
    if (!PyList_Check(lst)) {
        PyErr_SetString(PyExc_TypeError, "Expected list of tuples");
        return NULL;
    }
    std::vector<TempoMapEntry> entries;
    Py_ssize_t n = PyList_Size(lst);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PyList_GetItem(lst, i);
        double s, e, b1, b2;
        // Support (start, end, bpm) or (start, end, start_bpm, end_bpm)
        if (PyTuple_Size(item) >= 4) {
            if (!PyArg_ParseTuple(item, "dddd", &s, &e, &b1, &b2)) return NULL;
        } else {
            if (!PyArg_ParseTuple(item, "ddd", &s, &e, &b1)) return NULL;
            b2 = b1;
        }
        entries.push_back({s, e, b1, b2});
    }
    { CSLock lk(g.tmap_mtx); g.tempo_map = std::move(entries); }
    Py_RETURN_NONE;
}
static PyObject* py_set_tmap_active(PyObject*, PyObject* args) {
    int a; if (!PyArg_ParseTuple(args, "p", &a)) return NULL;
    g.tempo_map_active.store(a != 0);
    Py_RETURN_NONE;
}

// -- Tap tempo (native) -----------------------------------------------
static PyObject* py_tap_tempo(PyObject*, PyObject*) {
    double ts = (double)g.total_frames.load() / g.sample_rate;
    // If engine not running, use a clock
    if (!g.running.load()) {
        // fallback: monotonic-ish via Python time
        ts = 0; // caller should provide timestamp
    }
    { CSLock lk(g.tap_mtx); g.tap_det.tap(ts); }
    double bpm = g.tap_det.get_bpm();
    double stab = g.tap_det.get_stability();
    return Py_BuildValue("(dd)", bpm, stab);
}

static PyObject* py_tap_tempo_ts(PyObject*, PyObject* args) {
    double ts;
    if (!PyArg_ParseTuple(args, "d", &ts)) return NULL;
    { CSLock lk(g.tap_mtx); g.tap_det.tap(ts); }
    double bpm = g.tap_det.get_bpm();
    double stab = g.tap_det.get_stability();
    return Py_BuildValue("(dd)", bpm, stab);
}

static PyObject* py_tap_reset(PyObject*, PyObject*) {
    { CSLock lk(g.tap_mtx); g.tap_det.reset(); }
    Py_RETURN_NONE;
}

// -- Device enumeration -----------------------------------------------
static PyObject* py_list_devices(PyObject*, PyObject*) {
    if (!g.ctx_inited) {
        if (ma_context_init(NULL, 0, NULL, g.ctx) != MA_SUCCESS) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to init context");
            return NULL;
        }
        g.ctx_inited = true;
    }
    ma_device_info *pb, *cap;
    ma_uint32 pb_n, cap_n;
    ma_result result = ma_context_get_devices(g.ctx, &pb, &pb_n, &cap, &cap_n);
    if (result != MA_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to enumerate");
        return NULL;
    }
    PyObject* result_dict = PyDict_New();
    PyObject* pb_list = PyList_New(pb_n);
    for (ma_uint32 i = 0; i < pb_n; ++i) {
        PyObject* d = PyDict_New();
        PyDict_SetItemString(d, "name",  PyUnicode_FromString(pb[i].name));
        PyDict_SetItemString(d, "index", PyLong_FromLong(i));
        PyDict_SetItemString(d, "is_default", PyBool_FromLong(pb[i].isDefault));
        PyList_SetItem(pb_list, i, d);
    }
    PyDict_SetItemString(result_dict, "playback", pb_list);
    PyObject* cap_list = PyList_New(cap_n);
    for (ma_uint32 i = 0; i < cap_n; ++i) {
        PyObject* d = PyDict_New();
        PyDict_SetItemString(d, "name",  PyUnicode_FromString(cap[i].name));
        PyDict_SetItemString(d, "index", PyLong_FromLong(i));
        PyDict_SetItemString(d, "is_default", PyBool_FromLong(cap[i].isDefault));
        PyList_SetItem(cap_list, i, d);
    }
    PyDict_SetItemString(result_dict, "capture", cap_list);
    return result_dict;
}

// -- Resample (native) ------------------------------------------------
static PyObject* py_resample(PyObject*, PyObject* args) {
    PyObject* obj;
    int src_sr, dst_sr;
    if (!PyArg_ParseTuple(args, "Oii", &obj, &src_sr, &dst_sr)) return NULL;
    Py_buffer v;
    if (PyObject_GetBuffer(obj, &v, PyBUF_SIMPLE) != 0) return NULL;
    float* p = (float*)v.buf;
    size_t len = v.len / sizeof(float);
    auto out = resample(p, len, (uint32_t)src_sr, (uint32_t)dst_sr);
    PyBuffer_Release(&v);
    // Return as bytes (caller wraps in numpy)
    return PyBytes_FromStringAndSize((const char*)out.data(),
                                     out.size() * sizeof(float));
}

// -- Offline WAV export (native) --------------------------------------
static PyObject* py_export_wav(PyObject*, PyObject* args) {
    const char* filepath;
    double duration;
    double bpm, scale_v;
    int subdivision, ts_top;
    PyObject *accent_obj, *normal_obj;

    if (!PyArg_ParseTuple(args, "sdddiOO", &filepath, &duration, &bpm,
                          &scale_v, &subdivision, &ts_top,
                          &accent_obj, &normal_obj))
        return NULL;

    Py_buffer va, vn;
    if (PyObject_GetBuffer(accent_obj, &va, PyBUF_SIMPLE) != 0) return NULL;
    if (PyObject_GetBuffer(normal_obj, &vn, PyBUF_SIMPLE) != 0) {
        PyBuffer_Release(&va); return NULL;
    }

    float* pa = (float*)va.buf;
    size_t a_len = va.len / sizeof(float);
    float* pn = (float*)vn.buf;
    size_t n_len = vn.len / sizeof(float);

    uint32_t sr = g.sample_rate;
    size_t total_samples = (size_t)(sr * duration);
    std::vector<float> track(total_samples, 0.f);

    int cur_beat = 0, cur_sub = 0;
    double t = 0.0;

    while (t < duration) {
        bool is_accent = (cur_beat == 0 && cur_sub == 0);
        const float* smp = is_accent ? pa : pn;
        size_t smp_len    = is_accent ? a_len : n_len;

        size_t start_idx = (size_t)(t * (double)sr);
        for (size_t j = 0; j < smp_len && (start_idx + j) < total_samples; ++j)
            track[start_idx + j] += smp[j];

        double interval = 60.0 / (bpm * scale_v * subdivision);
        t += interval;

        cur_sub++;
        if (cur_sub >= subdivision) {
            cur_sub = 0;
            cur_beat = (cur_beat + 1) % ts_top;
        }
    }

    // Normalize
    float peak = 0.f;
    for (auto x : track) { float ax = fabsf(x); if (ax > peak) peak = ax; }
    if (peak > 1.f) for (auto& x : track) x /= peak;

    // Write WAV via miniaudio encoder
    ma_encoder_config enc_cfg = ma_encoder_config_init(
        ma_encoding_format_wav, ma_format_f32, 1, sr);
    ma_encoder encoder;
    if (ma_encoder_init_file(filepath, &enc_cfg, &encoder) != MA_SUCCESS) {
        PyBuffer_Release(&va);
        PyBuffer_Release(&vn);
        PyErr_SetString(PyExc_RuntimeError, "Failed to open WAV file for writing");
        return NULL;
    }
    ma_encoder_write_pcm_frames(&encoder, track.data(), total_samples, NULL);
    ma_encoder_uninit(&encoder);

    PyBuffer_Release(&va);
    PyBuffer_Release(&vn);
    Py_RETURN_TRUE;
}

// -- Tuner ------------------------------------------------------------
static PyObject* py_set_tuner_active(PyObject*, PyObject* args) {
    int active;
    if (!PyArg_ParseTuple(args, "p", &active)) return NULL;
    g.tuner.enabled = (active != 0);
    if (!g.tuner.enabled) g.tuner.current_hz.store(0.0f);
    Py_RETURN_NONE;
}

static PyObject* py_get_pitch_hz(PyObject*, PyObject*) {
    float hz = g.tuner.current_hz.load();
    return PyFloat_FromDouble(hz);
}

static PyObject* py_get_tuner_confidence(PyObject*, PyObject*) {
    float c = g.tuner.confidence.load();
    return PyFloat_FromDouble(c);
}

// -- Free Play Tracker ------------------------------------------------
static PyObject* py_set_free_play_active(PyObject*, PyObject* args) {
    int active;
    if (!PyArg_ParseTuple(args, "p", &active)) return NULL;
    if (active) {
        g.free_play.reset();
        g.free_play.active.store(true);
        // Also enable analysis so onset detection runs
        g.analyzing.store(true);
    } else {
        g.free_play.active.store(false);
    }
    Py_RETURN_NONE;
}

static PyObject* py_get_free_play_data(PyObject*, PyObject*) {
    // Returns dict with all free play stats
    PyObject* d = PyDict_New();
    PyDict_SetItemString(d, "live_bpm",   PyFloat_FromDouble(g.free_play.live_bpm.load()));
    PyDict_SetItemString(d, "stability",  PyFloat_FromDouble(g.free_play.stability.load()));
    PyDict_SetItemString(d, "min_bpm",    PyFloat_FromDouble(g.free_play.min_bpm.load()));
    PyDict_SetItemString(d, "max_bpm",    PyFloat_FromDouble(g.free_play.max_bpm.load()));
    PyDict_SetItemString(d, "avg_bpm",    PyFloat_FromDouble(g.free_play.avg_bpm.load()));
    PyDict_SetItemString(d, "drift",      PyLong_FromLong(g.free_play.drift_direction.load()));
    PyDict_SetItemString(d, "total_hits", PyLong_FromLong(g.free_play.total_hits.load()));
    return d;
}

// -- Drum mode --------------------------------------------------------
static PyObject* py_set_drum_mode(PyObject*, PyObject* args) {
    int dm;
    if (!PyArg_ParseTuple(args, "p", &dm)) return NULL;
    g.drum_mode.store(dm != 0);
    if (dm) {
        // Drum-optimized onset: lower debounce, lower threshold
        g.onset.debounce_min_sec    = 0.025;  // 25ms (was 40ms) — drums have sharp transients
        g.onset.bpm_debounce_factor = 0.25;   // (was 0.35) — allow faster recovery
        g.onset.base_threshold      = 0.06;   // (was 0.08) — drums are louder
        g.onset.flux_threshold      = 0.015;  // (was 0.02) — more sensitive to attacks
    } else {
        // Guitar/default parameters
        g.onset.debounce_min_sec    = 0.04;
        g.onset.bpm_debounce_factor = 0.35;
        g.onset.base_threshold      = 0.08;
        g.onset.flux_threshold      = 0.02;
    }
    Py_RETURN_NONE;
}

static PyObject* py_poll_clicks(PyObject*, PyObject*) {
    PyObject* list = PyList_New(0);
    EnterCriticalSection(&g.click_mtx);
    while (!g.click_queue.empty()) {
        ClickEvent ev = g.click_queue.front();
        g.click_queue.pop_front();
        PyObject* item = Py_BuildValue("(ii)", ev.beat, ev.substep);
        PyList_Append(list, item);
        Py_DECREF(item);
    }
    LeaveCriticalSection(&g.click_mtx);
    return list;
}

static PyObject* py_set_beat_pattern(PyObject*, PyObject* args) {
    PyObject* list_obj;
    if (!PyArg_ParseTuple(args, "O!", &PyList_Type, &list_obj)) return NULL;

    std::vector<int> new_pat;
    Py_ssize_t size = PyList_Size(list_obj);
    for (Py_ssize_t i = 0; i < size; ++i) {
        PyObject* item = PyList_GetItem(list_obj, i);
        new_pat.push_back((int)PyLong_AsLong(item));
    }

    {
        EnterCriticalSection(&g.pattern_mtx);
        g.beat_pattern = new_pat;
        LeaveCriticalSection(&g.pattern_mtx);
    }
    Py_RETURN_NONE;
}

// -- Tempo Coaching ---------------------------------------------------
static PyObject* py_get_tempo_coaching(PyObject*, PyObject*) {
    CSLock lk(g.coach_mtx);
    PyObject* d = PyDict_New();
    PyDict_SetItemString(d, "total_hits",       PyLong_FromLong(g.tempo_coach.total_hits));
    PyDict_SetItemString(d, "perfect_hits",     PyLong_FromLong(g.tempo_coach.perfect_hits));
    PyDict_SetItemString(d, "good_hits",        PyLong_FromLong(g.tempo_coach.good_hits));
    PyDict_SetItemString(d, "ok_hits",          PyLong_FromLong(g.tempo_coach.ok_hits));
    PyDict_SetItemString(d, "miss_hits",        PyLong_FromLong(g.tempo_coach.miss_hits));
    PyDict_SetItemString(d, "avg_deviation_ms", PyFloat_FromDouble(g.tempo_coach.avg_deviation_ms));
    PyDict_SetItemString(d, "advice_level",     PyLong_FromLong(g.tempo_coach.advice_level.load()));
    double perfect_pct = (g.tempo_coach.total_hits > 0)
        ? (double)g.tempo_coach.perfect_hits / g.tempo_coach.total_hits * 100.0 : 0.0;
    PyDict_SetItemString(d, "perfect_pct",      PyFloat_FromDouble(perfect_pct));
    return d;
}

static PyObject* py_reset_tempo_coaching(PyObject*, PyObject* args) {
    double bpm = g.bpm.load();
    if (PyTuple_Size(args) > 0) {
        if (!PyArg_ParseTuple(args, "d", &bpm)) return NULL;
    }
    CSLock lk(g.coach_mtx);
    g.tempo_coach.reset(bpm);
    Py_RETURN_NONE;
}

static PyObject* py_get_logs(PyObject*, PyObject*) {
    PyObject* list = PyList_New(0);
    EnterCriticalSection(&g.log_mtx);
    while (!g.log_queue.empty()) {
        PyObject* s = PyUnicode_FromString(g.log_queue.front().c_str());
        PyList_Append(list, s);
        Py_DECREF(s);
        g.log_queue.pop_front();
    }
    LeaveCriticalSection(&g.log_mtx);
    return list;
}

static PyObject* py_get_training_status(PyObject*, PyObject*) {
    PyObject* d = PyDict_New();
    
    // BPMLadder
    PyObject* ladder = PyDict_New();
    PyDict_SetItemString(ladder, "enabled", PyBool_FromLong(g.ladder.enabled.load()));
    PyDict_SetItemString(ladder, "measure_count", PyLong_FromLong(g.ladder.measure_count.load()));
    PyDict_SetItemString(ladder, "measures_per_step", PyLong_FromLong(g.ladder.measures_per_step.load()));
    PyDict_SetItemString(d, "ladder", ladder);
    
    // Disappearing
    PyObject* dis = PyDict_New();
    PyDict_SetItemString(dis, "enabled", PyBool_FromLong(g.disappearing.enabled.load()));
    PyDict_SetItemString(dis, "is_silent", PyBool_FromLong(g.disappearing.is_silent.load()));
    PyDict_SetItemString(dis, "measure_count", PyLong_FromLong(g.disappearing.measure_count.load()));
    PyDict_SetItemString(d, "disappearing", dis);

    // Human
    PyObject* human = PyDict_New();
    PyDict_SetItemString(human, "enabled", PyBool_FromLong(g.human.enabled.load()));
    PyDict_SetItemString(human, "is_silent", PyBool_FromLong(g.human.is_silent.load()));
    PyDict_SetItemString(human, "measure_count", PyLong_FromLong(g.human.measure_count.load()));
    PyDict_SetItemString(d, "human", human);
    
    // Boss
    PyObject* boss = PyDict_New();
    PyDict_SetItemString(boss, "enabled", PyBool_FromLong(g.boss.enabled.load()));
    PyDict_SetItemString(boss, "level", PyLong_FromLong(g.boss.level.load()));
    PyDict_SetItemString(boss, "is_silent", PyBool_FromLong(g.boss.is_silent.load()));
    PyDict_SetItemString(d, "boss", boss);

    return d;
}

// -- Training Modules -------------------------------------------------
static PyObject* py_set_ladder_params(PyObject*, PyObject* args) {
    int enabled, inc, mps, target;
    if (!PyArg_ParseTuple(args, "piii", &enabled, &inc, &mps, &target)) return NULL;
    
    bool was_enabled = g.ladder.enabled.load();
    bool now_enabled = (enabled != 0);
    
    g.ladder.enabled.store(now_enabled);
    g.ladder.increment.store(inc);
    g.ladder.measures_per_step.store(mps);
    g.ladder.target_bpm.store(target);
    
    // ONLY reset if we're turning it on for the first time
    if (now_enabled && !was_enabled) {
        g.ladder.reset();
    }
    Py_RETURN_NONE;
}

static PyObject* py_set_disappearing_params(PyObject*, PyObject* args) {
    int enabled, vis, invis;
    if (!PyArg_ParseTuple(args, "pii", &enabled, &vis, &invis)) return NULL;
    
    bool was_enabled = g.disappearing.enabled.load();
    bool now_enabled = (enabled != 0);
    
    g.disappearing.enabled.store(now_enabled);
    g.disappearing.visible_measures.store(vis);
    g.disappearing.invisible_measures.store(invis);
    
    if (now_enabled && !was_enabled) {
        g.disappearing.reset();
    }
    Py_RETURN_NONE;
}

static PyObject* py_set_random_silence_params(PyObject*, PyObject* args) {
    int enabled;
    float prob;
    if (!PyArg_ParseTuple(args, "pf", &enabled, &prob)) return NULL;
    g.random_silence.enabled.store(enabled != 0);
    g.random_silence.probability.store(prob);
    Py_RETURN_NONE;
}

static PyObject* py_set_offbeat_params(PyObject*, PyObject* args) {
    int enabled;
    if (!PyArg_ParseTuple(args, "p", &enabled)) return NULL;
    g.offbeat.enabled.store(enabled != 0);
    Py_RETURN_NONE;
}

static PyObject* py_set_boss_params(PyObject*, PyObject* args) {
    int enabled, level;
    if (!PyArg_ParseTuple(args, "pi", &enabled, &level)) return NULL;
    
    bool was_enabled = g.boss.enabled.load();
    bool now_enabled = (enabled != 0);
    
    g.boss.enabled.store(now_enabled);
    g.boss.level.store(level);
    
    if (now_enabled && !was_enabled) {
        g.boss.reset();
    }
    Py_RETURN_NONE;
}

static PyObject* py_set_human_params(PyObject*, PyObject* args) {
    int enabled, play, test;
    if (!PyArg_ParseTuple(args, "pii", &enabled, &play, &test)) return NULL;
    
    bool was_enabled = g.human.enabled.load();
    bool now_enabled = (enabled != 0);
    
    g.human.enabled.store(now_enabled);
    g.human.play_bars.store(play);
    g.human.test_bars.store(test);
    
    if (now_enabled && !was_enabled) {
        g.human.reset();
    }
    Py_RETURN_NONE;
}

// =====================================================================
//  Module table
// =====================================================================

static PyMethodDef methods[] = {
    {"init",               py_init,          METH_VARARGS, "init(sr)"},
    {"start",              py_start,         METH_NOARGS,  "Start engine"},
    {"stop",               py_stop,          METH_NOARGS,  "Stop engine"},
    {"cleanup",            py_cleanup,       METH_NOARGS,  "Cleanup engine"},
    {"set_bpm",            py_set_bpm,       METH_VARARGS, "set_bpm(bpm)"},
    {"set_scale",          py_set_scale,     METH_VARARGS, "set_scale(s)"},
    {"set_subdivision",    py_set_subdivision, METH_VARARGS, "set_subdivision(n)"},
    {"set_time_signature", py_set_time_sig,  METH_VARARGS, "set_time_signature(top)"},
    {"set_muted",          py_set_muted,     METH_VARARGS, "set_muted(b)"},
    {"set_groove_shift",   py_set_groove_shift, METH_VARARGS, "set_groove_shift(ms)"},
    {"set_samples",        py_set_samples,   METH_VARARGS, "set_samples(acc,norm)"},
    {"play_sound",         py_play_sound,    METH_VARARGS, "play_sound(buf)"},
    {"set_analysis",       py_set_analysis,  METH_VARARGS, "set_analysis(b)"},
    {"set_threshold",      py_set_threshold, METH_VARARGS, "set_threshold(v)"},
    {"set_onset_params",   py_set_onset_params, METH_VARARGS, "set_onset_params(...)"},
    {"poll_hits",          py_poll_hits,     METH_NOARGS,  "Poll hit events"},
    {"get_beat_pos",       py_get_beat_pos,  METH_NOARGS,  "Get beat position"},
    {"get_elapsed_sec",    py_get_elapsed,   METH_NOARGS,  "Elapsed seconds"},
    {"is_running",         py_is_running,    METH_NOARGS,  "Running?"},
    {"get_current_bpm",    py_get_bpm,       METH_NOARGS,  "Current BPM"},
    {"get_click_count",    py_get_click_count, METH_NOARGS, "Total clicks"},
    {"get_input_peak",     py_get_input_peak, METH_NOARGS, "Input peak level"},
    {"get_output_peak",    py_get_output_peak, METH_NOARGS, "Output peak level"},
    {"set_monitoring",     py_set_monitoring, METH_VARARGS, "set_monitoring(b, gain)"},
    {"add_polyrhythm",     py_add_poly,      METH_VARARGS, "add_poly(r,bpm,ts)"},
    {"set_tempo_map",      py_set_tempo_map, METH_VARARGS, "set_tempo_map(list)"},
    {"set_tempo_map_active",py_set_tmap_active,METH_VARARGS,"set_tmap_active(b)"},
    {"tap_tempo",          py_tap_tempo,     METH_NOARGS,  "Tap -> (bpm, stab)"},
    {"tap_tempo_ts",       py_tap_tempo_ts,  METH_VARARGS, "tap_tempo_ts(ts)"},
    {"tap_reset",          py_tap_reset,     METH_NOARGS,  "Reset tap detector"},
    {"list_devices",       py_list_devices,  METH_NOARGS,  "List audio devices"},
    {"resample",           py_resample,      METH_VARARGS, "resample(buf,src,dst)"},
    {"export_wav",         py_export_wav,    METH_VARARGS, "export_wav(...)"},
    {"set_tuner_active",   py_set_tuner_active, METH_VARARGS, "set_tuner_active(b)"},
    {"get_pitch_hz",       py_get_pitch_hz,  METH_NOARGS,  "Get detected pitch Hz"},
    {"get_tuner_confidence",py_get_tuner_confidence,METH_NOARGS,"Get tuner confidence 0-1"},
    {"set_free_play_active",py_set_free_play_active,METH_VARARGS,"set_free_play_active(b)"},
    {"get_free_play_data", py_get_free_play_data,METH_NOARGS,"Get free play stats dict"},
    {"set_drum_mode",      py_set_drum_mode, METH_VARARGS, "set_drum_mode(b)"},
    {"poll_clicks",        py_poll_clicks,   METH_NOARGS,  "Poll click event queue"},
    {"set_beat_pattern",   py_set_beat_pattern, METH_VARARGS, "set_beat_pattern(list)"},
    {"get_tempo_coaching", py_get_tempo_coaching, METH_NOARGS, "Get tempo coaching data"},
    {"reset_tempo_coaching",py_reset_tempo_coaching,METH_VARARGS,"Reset tempo coach"},
    {"get_logs",           py_get_logs,      METH_NOARGS,  "Get engine logs"},
    {"get_training_status",py_get_training_status, METH_NOARGS, "Get training modules status"},
    {"set_ladder_params",  py_set_ladder_params, METH_VARARGS, "set_ladder_params(en,inc,mps,target)"},
    {"set_disappearing_params", py_set_disappearing_params, METH_VARARGS, "set_disappearing_params(en,vis,invis)"},
    {"set_random_silence_params", py_set_random_silence_params, METH_VARARGS, "set_random_silence_params(en,prob)"},
    {"set_offbeat_params",  py_set_offbeat_params,  METH_VARARGS, "set_offbeat_params(en)"},
    {"set_boss_params",     py_set_boss_params,     METH_VARARGS, "set_boss_params(en,lvl)"},
    {"set_human_params",    py_set_human_params,    METH_VARARGS, "set_human_params(en,play,test)"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "omnibeat_ext",
    "Omnibeat native C++ audio engine v2.\n"
    "Sample-accurate metronome, smart onset detection (tremolo-safe),\n"
    "polyrhythm, groove-shift, tempo-map with ramps, tap-tempo,\n"
    "offline WAV export, resampling, and device enumeration.",
    -1,
    methods
};

PyMODINIT_FUNC PyInit_omnibeat_ext(void) {
    if (!g_ptr) g_ptr = new Engine();
    return PyModule_Create(&module_def);
}
