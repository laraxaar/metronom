#include "PolyrhythmEngine.h"
#include <cmath>
#include <algorithm>

void PolyrhythmEngine::addVoice(int ratio, double masterBpm, int bpmTop, uint64_t currentFrame, const std::vector<float>& sample, uint32_t sampleRate) {
    if (ratio <= 0 || masterBpm <= 0) return;

    double measureDur = (60.0 / masterBpm) * bpmTop;
    double intervalSec = measureDur / ratio;

    PolyVoice v;
    v.ratio = ratio;
    v.bpm = masterBpm;
    v.bpmTop = bpmTop;
    v.intervalFrames = intervalSec * sampleRate;
    v.nextFrame = (double)currentFrame;
    v.clicksRemaining = ratio;
    v.sample = sample;
    v.active = true;

    m_voices.push_back(std::move(v));
}

void PolyrhythmEngine::process(uint32_t nFrames, uint64_t startFrame, std::vector<std::pair<uint32_t, std::vector<float>>>& outTriggers) {
    uint64_t endFrame = startFrame + nFrames;

    for (auto it = m_voices.begin(); it != m_voices.end(); ) {
        if (!it->active) {
            it = m_voices.erase(it);
            continue;
        }

        while (it->nextFrame < (double)endFrame && it->clicksRemaining > 0) {
            uint32_t offset = (it->nextFrame < (double)startFrame) ? 0 : static_cast<uint32_t>(it->nextFrame - startFrame);
            outTriggers.push_back({offset, it->sample});
            
            it->nextFrame += it->intervalFrames;
            it->clicksRemaining--;
        }

        if (it->clicksRemaining <= 0) {
            it = m_voices.erase(it);
        } else {
            ++it;
        }
    }
}
