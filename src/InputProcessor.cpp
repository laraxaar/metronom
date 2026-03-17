#include "InputProcessor.h"
#include <cmath>
#include <algorithm>

InputProcessor::InputProcessor(size_t ringBufferCapacity)
    : m_ringBuffer(ringBufferCapacity)
{
}

void InputProcessor::initialize(uint32_t sampleRate, uint32_t totalInputChannels) {
    m_sampleRate = sampleRate;
    m_totalInputChannels = (totalInputChannels > 0) ? totalInputChannels : 1;

    // Set DC-offset filter cutoff: ~1.5 Hz is safe for all bass instruments
    // Low B on 5-string = 30.87 Hz, well above our cutoff
    m_dcFilter.setCutoff(1.5f, static_cast<float>(sampleRate));
    m_dcFilter.reset();

    // Pre-allocate channel extraction buffer for the maximum expected block size
    // 2048 frames is generous for most audio drivers
    m_channelExtractBuffer.resize(2048, 0.0f);

    m_ringBuffer.reset();
    m_pitchResult.reset();
    m_currentRMS.store(0.0f);
    m_peakLevel.store(0.0f);
}

void InputProcessor::pushSamples(const float* interleavedData, uint32_t nFrames) {
    if (!interleavedData || nFrames == 0) return;

    const uint32_t channel = m_inputChannel.load(std::memory_order_relaxed);
    const uint32_t totalCh = m_totalInputChannels;

    // Ensure extraction buffer is large enough
    if (m_channelExtractBuffer.size() < nFrames) {
        m_channelExtractBuffer.resize(nFrames);
    }

    // Extract the selected channel from interleaved data
    if (totalCh == 1) {
        // Mono — direct copy
        for (uint32_t i = 0; i < nFrames; ++i) {
            m_channelExtractBuffer[i] = interleavedData[i];
        }
    } else {
        // Multi-channel — extract specific channel
        const uint32_t ch = (channel < totalCh) ? channel : 0;
        for (uint32_t i = 0; i < nFrames; ++i) {
            m_channelExtractBuffer[i] = interleavedData[i * totalCh + ch];
        }
    }

    // Apply DC-offset filter in-place
    m_dcFilter.process(m_channelExtractBuffer.data(), nFrames);

    // Calculate RMS and peak for metering
    float sumSq = 0.0f;
    float peak = 0.0f;
    for (uint32_t i = 0; i < nFrames; ++i) {
        const float s = m_channelExtractBuffer[i];
        sumSq += s * s;
        const float absS = std::abs(s);
        if (absS > peak) peak = absS;
    }

    float rms = std::sqrt(sumSq / static_cast<float>(nFrames));
    m_currentRMS.store(rms, std::memory_order_relaxed);

    // Smooth peak decay
    float prevPeak = m_peakLevel.load(std::memory_order_relaxed);
    if (peak > prevPeak) {
        m_peakLevel.store(peak, std::memory_order_relaxed);
    } else {
        m_peakLevel.store(prevPeak * 0.95f, std::memory_order_relaxed);
    }

    // Write filtered samples to ring buffer
    m_ringBuffer.write(m_channelExtractBuffer.data(), nFrames);
}

void InputProcessor::pushMonoSamples(const float* monoData, uint32_t nFrames) {
    if (!monoData || nFrames == 0) return;

    // Ensure extraction buffer is large enough
    if (m_channelExtractBuffer.size() < nFrames) {
        m_channelExtractBuffer.resize(nFrames);
    }

    // Copy to temporary buffer for in-place DC filtering
    for (uint32_t i = 0; i < nFrames; ++i) {
        m_channelExtractBuffer[i] = monoData[i];
    }

    m_dcFilter.process(m_channelExtractBuffer.data(), nFrames);

    // Metering
    float sumSq = 0.0f;
    float peak = 0.0f;
    for (uint32_t i = 0; i < nFrames; ++i) {
        const float s = m_channelExtractBuffer[i];
        sumSq += s * s;
        const float absS = std::abs(s);
        if (absS > peak) peak = absS;
    }

    float rms = std::sqrt(sumSq / static_cast<float>(nFrames));
    m_currentRMS.store(rms, std::memory_order_relaxed);

    float prevPeak = m_peakLevel.load(std::memory_order_relaxed);
    if (peak > prevPeak) {
        m_peakLevel.store(peak, std::memory_order_relaxed);
    } else {
        m_peakLevel.store(prevPeak * 0.95f, std::memory_order_relaxed);
    }

    m_ringBuffer.write(m_channelExtractBuffer.data(), nFrames);
}

size_t InputProcessor::getAnalysisWindow(float* dest, size_t windowSize) const {
    return m_ringBuffer.peek(dest, windowSize, 0);
}

size_t InputProcessor::getAnalysisWindowWithHop(float* dest, size_t windowSize, size_t hopSize) const {
    return m_ringBuffer.peek(dest, windowSize, hopSize);
}

void InputProcessor::advanceReadPosition(size_t count) {
    m_ringBuffer.skip(count);
}

bool InputProcessor::isWindowAvailable(size_t windowSize) const {
    return m_ringBuffer.availableRead() >= windowSize;
}

size_t InputProcessor::availableSamples() const {
    return m_ringBuffer.availableRead();
}

size_t InputProcessor::recommendedWindowSize(float targetFreqHz, int numPeriods) const {
    if (targetFreqHz <= 0.0f) return 8192; // Safe default

    // Period duration in samples = sampleRate / targetFreq
    // We need numPeriods complete periods for robust pitch detection
    float periodSamples = static_cast<float>(m_sampleRate) / targetFreqHz;
    size_t needed = static_cast<size_t>(std::ceil(periodSamples * numPeriods));

    // Round up to next power of two for FFT compatibility
    size_t pot = 1;
    while (pot < needed) pot <<= 1;

    return pot;
}

void InputProcessor::setInputChannel(uint32_t channel) {
    m_inputChannel.store(channel, std::memory_order_relaxed);
}

void InputProcessor::reset() {
    m_ringBuffer.reset();
    m_dcFilter.reset();
    m_pitchResult.reset();
    m_currentRMS.store(0.0f, std::memory_order_relaxed);
    m_peakLevel.store(0.0f, std::memory_order_relaxed);
}
