#include "AudioMixer.h"
#include <cmath>
#include <algorithm>
#include <iostream>

// dr_wav (single translation unit)
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

AudioMixer::AudioMixer(MixerState& state) : m_state(state) {}

void AudioMixer::setSampleRate(uint32_t sr) {
    if (sr == 0) sr = 48000;
    m_sampleRate = sr;
}

bool AudioMixer::loadWavSample(SampleId id, const std::string& path) {
    unsigned int channels = 0;
    unsigned int sampleRate = 0;
    drwav_uint64 totalFrameCount = 0;

    float* p = drwav_open_file_and_read_pcm_frames_f32(
        path.c_str(), &channels, &sampleRate, &totalFrameCount, nullptr);
    if (!p || totalFrameCount == 0) {
        if (p) drwav_free(p, nullptr);
        return false;
    }

    std::vector<float> mono;
    mono.resize(static_cast<size_t>(totalFrameCount));

    if (channels == 1) {
        std::copy(p, p + totalFrameCount, mono.begin());
    } else {
        for (drwav_uint64 i = 0; i < totalFrameCount; ++i) {
            float sum = 0.0f;
            for (unsigned int c = 0; c < channels; ++c) sum += p[i * channels + c];
            mono[static_cast<size_t>(i)] = sum / static_cast<float>(channels);
        }
    }

    drwav_free(p, nullptr);

    // NOTE: no resample yet. If file SR differs, playback will be pitch/time shifted.
    // For now we accept it and keep engine SR consistent.
    (void)sampleRate;

    const int idx = static_cast<int>(id);
    if (idx < 0 || idx >= 5) return false;
    m_samples[idx].mono = std::move(mono);
    m_samples[idx].loaded = true;

    // Reserve tail capacity (control thread): max sample length across loaded samples
    uint32_t maxLen = m_tailCapacity;
    for (const auto& s : m_samples) {
        if (s.loaded) {
            maxLen = (std::max<uint32_t>)(maxLen, static_cast<uint32_t>(s.mono.size()));
        }
    }
    m_tailCapacity = maxLen;
    if (m_tailCapacity > 0) {
        m_clickTail.data.resize(m_tailCapacity);
        m_polyTail.data.resize(m_tailCapacity);
        m_clickTail.clear();
        m_polyTail.clear();
    }
    return true;
}

void AudioMixer::clearSample(SampleId id) {
    const int idx = static_cast<int>(id);
    if (idx < 0 || idx >= 5) return;
    m_samples[idx].mono.clear();
    m_samples[idx].loaded = false;
}

void AudioMixer::mixOneShotSample(
    const std::vector<float>& mono,
    uint32_t offset,
    float* out,
    uint32_t ch,
    uint32_t nFrames,
    float gain,
    TailBuffer& tail)
{
    if (mono.empty() || offset >= nFrames) return;

    const uint32_t framesAvail = nFrames - offset;
    const uint32_t framesToMix = (std::min<uint32_t>)(framesAvail, static_cast<uint32_t>(mono.size()));

    for (uint32_t i = 0; i < framesToMix; ++i) {
        const float s = mono[i] * gain;
        for (uint32_t c = 0; c < ch; ++c) out[(offset + i) * ch + c] += s;
    }

    if (framesToMix < mono.size()) {
        // Store scaled remaining into tail (RT-safe: preallocated)
        const uint32_t remain = static_cast<uint32_t>(mono.size()) - framesToMix;
        if (remain <= tail.data.size()) {
            tail.pos = 0;
            tail.len = remain;
            for (uint32_t i = 0; i < remain; ++i) {
                tail.data[i] = mono[framesToMix + i] * gain;
            }
        }
    }
}

void AudioMixer::computePeakRmsMono(const float* x, uint32_t n, float& outPeak, float& outRms) {
    float peak = 0.0f;
    double sumSq = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const float s = x[i];
        const float a = std::abs(s);
        if (a > peak) peak = a;
        sumSq += static_cast<double>(s) * static_cast<double>(s);
    }
    outPeak = peak;
    outRms = (n > 0) ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(n))) : 0.0f;
}

float AudioMixer::smoothPeak(float prev, float next) {
    if (next > prev) return next;
    return prev * 0.95f;
}

void AudioMixer::processBlock(
    const float* inputInterleaved,
    uint32_t numInputChannels,
    float* outputInterleaved,
    uint32_t numOutputChannels,
    uint32_t nFrames,
    const MetronomeEngine::Event* tickEvents,
    size_t numTickEvents,
    const std::vector<std::pair<uint32_t, std::vector<float>>>& polyTriggers,
    double bpmForFallbackSynth)
{
    const uint32_t outCh = (std::max<uint32_t>)(1, numOutputChannels);
    const uint32_t inCh = (std::max<uint32_t>)(1, numInputChannels);

    // Clear output
    for (uint32_t i = 0; i < nFrames * outCh; ++i) outputInterleaved[i] = 0.0f;

    // Drain tails (already scaled)
    auto drain = [&](TailBuffer& t) {
        const uint32_t avail = t.available();
        if (avail == 0) return;
        const uint32_t toDrain = (std::min<uint32_t>)(nFrames, avail);
        for (uint32_t i = 0; i < toDrain; ++i) {
            const float s = t.data[t.pos + i];
            for (uint32_t c = 0; c < outCh; ++c) outputInterleaved[i * outCh + c] += s;
        }
        t.pos += toDrain;
        if (t.pos >= t.len) t.clear();
    };
    drain(m_clickTail);
    drain(m_polyTail);

    // Scratch mono busses for metering (no allocations: fixed max for typical buffers; fallback allocate once)
    static thread_local std::vector<float> clickBus;
    static thread_local std::vector<float> accentBus;
    static thread_local std::vector<float> inputBus;
    if (clickBus.size() < nFrames) clickBus.assign(nFrames, 0.0f);
    else std::fill(clickBus.begin(), clickBus.begin() + nFrames, 0.0f);
    if (accentBus.size() < nFrames) accentBus.assign(nFrames, 0.0f);
    else std::fill(accentBus.begin(), accentBus.begin() + nFrames, 0.0f);
    if (inputBus.size() < nFrames) inputBus.assign(nFrames, 0.0f);
    else std::fill(inputBus.begin(), inputBus.begin() + nFrames, 0.0f);

    // Mix tick events (GridA: click/accent, GridB: cowbell by default)
    for (size_t k = 0; k < numTickEvents; ++k) {
        const uint32_t offset = tickEvents[k].sampleOffset;
        const float vel = tickEvents[k].velocity;
        if (offset >= nFrames) continue;

        const bool isGridB = (tickEvents[k].grid == MetronomeEngine::GridId::B);
        const bool isAccent = (!isGridB) && (vel > 0.9f);

        const SampleId sid = isGridB
            ? SampleId::Cowbell
            : (isAccent ? SampleId::ClickAccent : SampleId::ClickNormal);

        const auto& sdata = m_samples[static_cast<int>(sid)];
        const float clickGain = isGridB
            ? m_state.clickVolume.load(std::memory_order_relaxed) * 0.9f
            : (isAccent ? m_state.accentVolume.load(std::memory_order_relaxed)
                        : m_state.clickVolume.load(std::memory_order_relaxed));
        const float master = m_state.masterVolume.load(std::memory_order_relaxed);
        const float gain = clickGain * master * vel;

        if (sdata.loaded && !sdata.mono.empty()) {
            // mix directly into output and bus, tail is handled by helper
            const uint32_t framesAvail = nFrames - offset;
            const uint32_t framesToMix = (std::min<uint32_t>)(framesAvail, static_cast<uint32_t>(sdata.mono.size()));
            for (uint32_t i = 0; i < framesToMix; ++i) {
                const float s = sdata.mono[i] * gain;
                (isAccent ? accentBus : clickBus)[offset + i] += s;
                for (uint32_t c = 0; c < outCh; ++c) outputInterleaved[(offset + i) * outCh + c] += s;
            }
            if (framesToMix < sdata.mono.size()) {
                // scaled tail (remaining part only)
                const uint32_t remain = static_cast<uint32_t>(sdata.mono.size()) - framesToMix;
                if (remain <= m_clickTail.data.size()) {
                    m_clickTail.pos = 0;
                    m_clickTail.len = remain;
                    for (uint32_t i = 0; i < remain; ++i) {
                        m_clickTail.data[i] = sdata.mono[framesToMix + i] * gain;
                    }
                }
            }
        } else {
            // Fallback synthesized click (short sine burst)
            const float sr = static_cast<float>(m_sampleRate);
            const float bpm = static_cast<float>(bpmForFallbackSynth > 0.0 ? bpmForFallbackSynth : 120.0);
            const float durMs = (bpm > 400.0f) ? 5.0f : 15.0f;
            const uint32_t clickLen = static_cast<uint32_t>(sr * durMs * 0.001f);
            const float baseVol = isAccent ? 0.95f : 0.75f;
            const float freq = isAccent ? 2000.0f : 1200.0f;
            const float synthGain = baseVol * gain;

            for (uint32_t i = 0; i < clickLen && (offset + i) < nFrames; ++i) {
                const float t = static_cast<float>(i) / sr;
                const float phase = static_cast<float>(i) / static_cast<float>(clickLen);
                const float env = std::pow(0.5f * (1.0f - std::cos(2.0f * 3.14159f * phase)), 0.5f) * std::exp(-phase * 8.0f);
                const float s = std::sin(2.0f * 3.14159f * freq * t) * env * synthGain;
                (isAccent ? accentBus : clickBus)[offset + i] += s;
                for (uint32_t c = 0; c < outCh; ++c) outputInterleaved[(offset + i) * outCh + c] += s;
            }
        }
    }

    // Mix polyrhythm triggers (use provided sample if not empty, otherwise use Cowbell/Kick fallback)
    for (const auto& trig : polyTriggers) {
        const uint32_t offset = trig.first;
        if (offset >= nFrames) continue;

        const float master = m_state.masterVolume.load(std::memory_order_relaxed);
        const float gain = m_state.clickVolume.load(std::memory_order_relaxed) * master * 0.9f;

        const std::vector<float>* mono = &trig.second;
        if (mono->empty()) {
            const auto& fallback = m_samples[static_cast<int>(SampleId::Cowbell)];
            if (fallback.loaded) mono = &fallback.mono;
        }
        if (!mono->empty()) {
            const uint32_t framesAvail = nFrames - offset;
            const uint32_t framesToMix = (std::min<uint32_t>)(framesAvail, static_cast<uint32_t>(mono->size()));
            for (uint32_t i = 0; i < framesToMix; ++i) {
                const float s = (*mono)[i] * gain;
                clickBus[offset + i] += s;
                for (uint32_t c = 0; c < outCh; ++c) outputInterleaved[(offset + i) * outCh + c] += s;
            }
            if (framesToMix < mono->size()) {
                // scaled tail (copy remaining)
                const uint32_t remain = static_cast<uint32_t>(mono->size()) - framesToMix;
                if (remain <= m_polyTail.data.size()) {
                    m_polyTail.pos = 0;
                    m_polyTail.len = remain;
                    for (uint32_t i = 0; i < remain; ++i) {
                        m_polyTail.data[i] = (*mono)[framesToMix + i] * gain;
                    }
                }
            }
        }
    }

    // Monitoring mix (input ch0)
    if (inputInterleaved) {
        const bool mon = true;
        if (mon) {
            const float monGain = m_state.inputVolume.load(std::memory_order_relaxed)
                                * m_state.masterVolume.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < nFrames; ++i) {
                const float s = inputInterleaved[i * inCh] * monGain;
                inputBus[i] += s;
                for (uint32_t c = 0; c < outCh; ++c) outputInterleaved[i * outCh + c] += s;
            }
        }
    }

    // Metering (peak only stored in MixerState for UI; RMS can be added later)
    float clickPeak = 0.0f, clickRms = 0.0f;
    float accentPeak = 0.0f, accentRms = 0.0f;
    float inputPeak = 0.0f, inputRms = 0.0f;
    computePeakRmsMono(clickBus.data(), nFrames, clickPeak, clickRms);
    computePeakRmsMono(accentBus.data(), nFrames, accentPeak, accentRms);
    computePeakRmsMono(inputBus.data(), nFrames, inputPeak, inputRms);

    float masterPeak = 0.0f, masterRms = 0.0f;
    // approximate master mono by averaging channels
    static thread_local std::vector<float> masterBus;
    if (masterBus.size() < nFrames) masterBus.assign(nFrames, 0.0f);
    else std::fill(masterBus.begin(), masterBus.begin() + nFrames, 0.0f);
    for (uint32_t i = 0; i < nFrames; ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < outCh; ++c) sum += outputInterleaved[i * outCh + c];
        masterBus[i] = sum / static_cast<float>(outCh);
    }
    computePeakRmsMono(masterBus.data(), nFrames, masterPeak, masterRms);

    m_state.clickPeak.store(smoothPeak(m_state.clickPeak.load(std::memory_order_relaxed), std::max(clickPeak, accentPeak)), std::memory_order_relaxed);
    m_state.inputPeak.store(smoothPeak(m_state.inputPeak.load(std::memory_order_relaxed), inputPeak), std::memory_order_relaxed);
    m_state.masterPeak.store(smoothPeak(m_state.masterPeak.load(std::memory_order_relaxed), masterPeak), std::memory_order_relaxed);
}

