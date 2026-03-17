#include "MetronomeEngine.h"
#include "TempoMap.h"
#include "ITrainingModule.h"
#include <algorithm>
#include <cmath>
#include <memory>

static MetronomeEngine::StepState clampState(int v) {
    if (v <= 0) return MetronomeEngine::StepState::ACCENT;
    if (v == 1) return MetronomeEngine::StepState::NORMAL;
    return MetronomeEngine::StepState::MUTE;
}

MetronomeEngine::MetronomeEngine() {
    // default 4/4, quarters
    setSubdivisionParts(1);
    setBeatsPerBar(4);
    setBpm(120.0);
    reset();
    // default pattern: first step accent, others normal
    for (int i = 0; i < MAX_STEPS; ++i) {
        m_stepStates[i].store(static_cast<uint8_t>(StepState::MUTE), std::memory_order_relaxed);
    }
    int steps = m_stepsPerBar.load(std::memory_order_relaxed);
    if (steps > 0) {
        m_stepStates[0].store(static_cast<uint8_t>(StepState::ACCENT), std::memory_order_relaxed);
        for (int i = 1; i < steps; ++i) {
            m_stepStates[i].store(static_cast<uint8_t>(StepState::NORMAL), std::memory_order_relaxed);
        }
    }

    // Grid B defaults mirror A (but disabled)
    m_partsPerQuarterB.store(m_partsPerQuarter.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_stepsPerBarB.store(m_stepsPerBar.load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int i = 0; i < MAX_STEPS; ++i) {
        m_stepStatesB[i].store(static_cast<uint8_t>(StepState::MUTE), std::memory_order_relaxed);
    }
    if (steps > 0) {
        m_stepStatesB[0].store(static_cast<uint8_t>(StepState::ACCENT), std::memory_order_relaxed);
        for (int i = 1; i < steps; ++i) {
            m_stepStatesB[i].store(static_cast<uint8_t>(StepState::NORMAL), std::memory_order_relaxed);
        }
    }
}

void MetronomeEngine::setSampleRate(uint32_t sampleRate) {
    if (sampleRate == 0) sampleRate = 48000;
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
}

void MetronomeEngine::setTempoMap(std::shared_ptr<const TempoMap> map) {
    // C++17: atomic access via free functions (lock-free when supported).
    std::atomic_store_explicit(&m_tempoMap, std::move(map), std::memory_order_release);
}

void MetronomeEngine::setScaling(float factor) {
    if (!std::isfinite(factor)) factor = 1.0f;
    factor = std::clamp(factor, 0.01f, 4.0f);
    m_scaling.store(factor, std::memory_order_relaxed);
}

void MetronomeEngine::clearTrainingModules() {
    m_trainingModuleCount.store(0, std::memory_order_release);
    for (int i = 0; i < MAX_TRAINING_MODULES; ++i) {
        m_trainingModules[i] = nullptr;
    }
}

bool MetronomeEngine::addTrainingModule(ITrainingModule* module) {
    if (!module) return false;
    int n = m_trainingModuleCount.load(std::memory_order_acquire);
    if (n < 0) n = 0;
    if (n >= MAX_TRAINING_MODULES) return false;
    m_trainingModules[n] = module;
    m_trainingModuleCount.store(n + 1, std::memory_order_release);
    return true;
}

void MetronomeEngine::setBpm(double bpm) {
    if (!std::isfinite(bpm)) bpm = 120.0;
    bpm = std::clamp(bpm, 10.0, 1000.0);
    m_bpm.store(bpm, std::memory_order_relaxed);
}

int MetronomeEngine::coerceSubdivisionParts(int parts) {
    // allowed: 1,2,3,4,5,7. Anything else -> nearest sensible.
    switch (parts) {
        case 1: case 2: case 3: case 4: case 5: case 7: return parts;
        default: break;
    }
    if (parts <= 1) return 1;
    if (parts == 6) return 5;
    if (parts < 7) return 7;
    // beyond 7: keep at 7 (requested list is strict)
    return 7;
}

void MetronomeEngine::setBeatsPerBar(int beatsPerBar) {
    if (beatsPerBar < 1) beatsPerBar = 1;
    if (beatsPerBar > 64) beatsPerBar = 64;
    m_beatsPerBar.store(beatsPerBar, std::memory_order_relaxed);

    int parts = m_partsPerQuarter.load(std::memory_order_relaxed);
    int newSteps = std::clamp(beatsPerBar * parts, 1, MAX_STEPS);
    rebuildGridForNewSteps(newSteps);

    // Keep grid B steps consistent with its own parts-per-quarter
    const int partsB = m_partsPerQuarterB.load(std::memory_order_relaxed);
    const int newStepsB = std::clamp(beatsPerBar * partsB, 1, MAX_STEPS);
    m_stepsPerBarB.store(newStepsB, std::memory_order_relaxed);
}

void MetronomeEngine::setSubdivisionParts(int partsPerQuarter) {
    partsPerQuarter = coerceSubdivisionParts(partsPerQuarter);
    m_partsPerQuarter.store(partsPerQuarter, std::memory_order_relaxed);

    int beats = m_beatsPerBar.load(std::memory_order_relaxed);
    int newSteps = std::clamp(beats * partsPerQuarter, 1, MAX_STEPS);
    rebuildGridForNewSteps(newSteps);
}

void MetronomeEngine::setSubdivisionPartsB(int partsPerQuarter) {
    partsPerQuarter = coerceSubdivisionParts(partsPerQuarter);
    m_partsPerQuarterB.store(partsPerQuarter, std::memory_order_relaxed);
    const int beats = m_beatsPerBar.load(std::memory_order_relaxed);
    const int newSteps = std::clamp(beats * partsPerQuarter, 1, MAX_STEPS);

    // Resample grid B states proportionally (similar to A)
    const int oldSteps = m_stepsPerBarB.load(std::memory_order_relaxed);
    uint8_t oldBuf[MAX_STEPS];
    for (int i = 0; i < MAX_STEPS; ++i) oldBuf[i] = m_stepStatesB[i].load(std::memory_order_relaxed);
    for (int i = 0; i < newSteps; ++i) {
        int src = (oldSteps > 0) ? ((i * oldSteps) / newSteps) : 0;
        if (src < 0) src = 0;
        if (src >= oldSteps) src = (oldSteps > 0) ? (oldSteps - 1) : 0;
        m_stepStatesB[i].store(oldBuf[src], std::memory_order_relaxed);
    }
    for (int i = newSteps; i < MAX_STEPS; ++i) {
        m_stepStatesB[i].store(static_cast<uint8_t>(StepState::MUTE), std::memory_order_relaxed);
    }
    m_stepsPerBarB.store(newSteps, std::memory_order_relaxed);
    int cur = m_currentStepB.load(std::memory_order_relaxed);
    if (cur >= newSteps) m_currentStepB.store(0, std::memory_order_relaxed);
}

void MetronomeEngine::setGridBClicksPerBar(int clicks) {
    if (clicks < 0) clicks = 0;
    if (clicks > MAX_STEPS) clicks = MAX_STEPS;
    m_gridBClicksPerBar.store(clicks, std::memory_order_relaxed);
    if (clicks > 0) {
        m_stepsPerBarB.store(clicks, std::memory_order_relaxed);
        int cur = m_currentStepB.load(std::memory_order_relaxed);
        if (cur >= clicks) m_currentStepB.store(0, std::memory_order_relaxed);
    } else {
        // fallback to beats * partsPerQuarterB
        const int beats = m_beatsPerBar.load(std::memory_order_relaxed);
        const int partsB = m_partsPerQuarterB.load(std::memory_order_relaxed);
        m_stepsPerBarB.store(std::clamp(beats * partsB, 1, MAX_STEPS), std::memory_order_relaxed);
    }
}

void MetronomeEngine::rebuildGridForNewSteps(int newStepsPerBar) {
    int oldSteps = m_stepsPerBar.load(std::memory_order_relaxed);
    if (newStepsPerBar == oldSteps) return;

    // Snapshot old states (non-RT function, intended to be called from control thread).
    uint8_t oldBuf[MAX_STEPS];
    for (int i = 0; i < MAX_STEPS; ++i) {
        oldBuf[i] = m_stepStates[i].load(std::memory_order_relaxed);
    }

    // Resample states proportionally.
    for (int i = 0; i < newStepsPerBar; ++i) {
        int src = (oldSteps > 0) ? ((i * oldSteps) / newStepsPerBar) : 0;
        if (src < 0) src = 0;
        if (src >= oldSteps) src = (oldSteps > 0) ? (oldSteps - 1) : 0;
        m_stepStates[i].store(oldBuf[src], std::memory_order_relaxed);
    }
    for (int i = newStepsPerBar; i < MAX_STEPS; ++i) {
        m_stepStates[i].store(static_cast<uint8_t>(StepState::MUTE), std::memory_order_relaxed);
    }

    m_stepsPerBar.store(newStepsPerBar, std::memory_order_relaxed);

    int cur = m_currentStep.load(std::memory_order_relaxed);
    if (cur >= newStepsPerBar) m_currentStep.store(0, std::memory_order_relaxed);
}

void MetronomeEngine::setStepState(int stepIndex, StepState state) {
    int steps = m_stepsPerBar.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= steps) return;
    m_stepStates[stepIndex].store(static_cast<uint8_t>(state), std::memory_order_relaxed);
}

MetronomeEngine::StepState MetronomeEngine::getStepState(int stepIndex) const {
    int steps = m_stepsPerBar.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= steps) return StepState::MUTE;
    uint8_t v = m_stepStates[stepIndex].load(std::memory_order_relaxed);
    return clampState(static_cast<int>(v));
}

void MetronomeEngine::cycleStepState(int stepIndex) {
    int steps = m_stepsPerBar.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= steps) return;

    uint8_t oldV = m_stepStates[stepIndex].load(std::memory_order_relaxed);
    int s = static_cast<int>(oldV) % 3;
    int next = (s + 1) % 3;
    m_stepStates[stepIndex].store(static_cast<uint8_t>(next), std::memory_order_relaxed);
}

void MetronomeEngine::reset() {
    m_samplesUntilNextTick = 0.0;
    m_currentStep.store(0, std::memory_order_relaxed);
    m_totalSamples = 0;
    m_samplesUntilNextTickB = 0.0;
    m_currentStepB.store(0, std::memory_order_relaxed);
}

void MetronomeEngine::setStepStateB(int stepIndex, StepState state) {
    int steps = m_stepsPerBarB.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= steps) return;
    m_stepStatesB[stepIndex].store(static_cast<uint8_t>(state), std::memory_order_relaxed);
}

MetronomeEngine::StepState MetronomeEngine::getStepStateB(int stepIndex) const {
    int steps = m_stepsPerBarB.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= steps) return StepState::MUTE;
    uint8_t v = m_stepStatesB[stepIndex].load(std::memory_order_relaxed);
    return clampState(static_cast<int>(v));
}

void MetronomeEngine::cycleStepStateB(int stepIndex) {
    int steps = m_stepsPerBarB.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= steps) return;
    uint8_t oldV = m_stepStatesB[stepIndex].load(std::memory_order_relaxed);
    int s = static_cast<int>(oldV) % 3;
    int next = (s + 1) % 3;
    m_stepStatesB[stepIndex].store(static_cast<uint8_t>(next), std::memory_order_relaxed);
}

size_t MetronomeEngine::processBlock(uint32_t nFrames, Event* outEvents, size_t outCapacity) {
    if (!outEvents || outCapacity == 0 || nFrames == 0) return 0;

    const uint32_t sr = m_sampleRate.load(std::memory_order_relaxed);
    const int stepsPerBar = m_stepsPerBar.load(std::memory_order_relaxed);
    const int partsPerQuarter = m_partsPerQuarter.load(std::memory_order_relaxed);

    if (sr == 0 || stepsPerBar <= 0 || partsPerQuarter <= 0) return 0;

    size_t written = 0;

    // We maintain "samples until next tick". Decrease it by each processed sample offset.
    // When it reaches <= 0, emit tick(s) and push it forward by stepLen.
    double remainingA = m_samplesUntilNextTick;
    double remainingB = m_samplesUntilNextTickB;

    // If starting fresh or after reset, schedule immediate tick at offset 0.
    if (!std::isfinite(remainingA)) remainingA = 0.0;
    if (!std::isfinite(remainingB)) remainingB = 0.0;

    const bool gridBEnabled = m_gridBEnabled.load(std::memory_order_relaxed);
    const int stepsPerBarB = m_stepsPerBarB.load(std::memory_order_relaxed);
    const int partsPerQuarterB = m_partsPerQuarterB.load(std::memory_order_relaxed);
    const int clicksPerBarB = m_gridBClicksPerBar.load(std::memory_order_relaxed);

    uint32_t frame = 0;
    while (frame < nFrames) {
        // Decide how far to advance to next event (min of remainingA/remainingB if enabled)
        double next = remainingA;
        if (gridBEnabled) next = std::min(next, remainingB);

        if (next > 0.0) {
            const uint32_t advance = static_cast<uint32_t>(std::min<double>(next, static_cast<double>(nFrames - frame)));
            frame += advance;
            remainingA -= static_cast<double>(advance);
            if (gridBEnabled) remainingB -= static_cast<double>(advance);
            continue;
        }

        // Compute BPM for THIS tick time (tempo map + scaling), fall back to static bpm.
        const double tickTimeSec = static_cast<double>(m_totalSamples + frame) / static_cast<double>(sr);
        double bpmNow = m_bpm.load(std::memory_order_relaxed);
        if (auto map = std::atomic_load_explicit(&m_tempoMap, std::memory_order_acquire)) {
            bpmNow = map->bpmAt(tickTimeSec);
        }
        const float scale = m_scaling.load(std::memory_order_relaxed);
        bpmNow *= static_cast<double>(scale);
        if (!std::isfinite(bpmNow) || bpmNow <= 0.0) bpmNow = 120.0;
        bpmNow = std::clamp(bpmNow, 10.0, 1000.0);

        // NOTE: step length depends on BPM, which may be overridden by training modules.
        // We compute stepLenA/stepLenB after processing any BPM override for this tick.

        // Precompute for GridB scheduling if needed later (uses bpmNow before overrides)
        double stepLenA = 1.0;
        double stepLenB = 1.0;

        auto computeStepLens = [&]() {
            const double samplesPerQuarterNow = (static_cast<double>(sr) * 60.0) / bpmNow;
            const double barLen = samplesPerQuarterNow * static_cast<double>(m_beatsPerBar.load(std::memory_order_relaxed));
            stepLenA = std::clamp(samplesPerQuarterNow / static_cast<double>(partsPerQuarter), 1.0, 1.0e9);
            stepLenB = (clicksPerBarB > 0)
                ? std::clamp(barLen / static_cast<double>(clicksPerBarB), 1.0, 1.0e9)
                : std::clamp(samplesPerQuarterNow / static_cast<double>(partsPerQuarterB > 0 ? partsPerQuarterB : 1), 1.0, 1.0e9);
        };
        computeStepLens();

        // Emit A if due
        if (remainingA <= 0.0) {
            int stepIdx = m_currentStep.load(std::memory_order_relaxed);
            if (stepIdx < 0 || stepIdx >= stepsPerBar) stepIdx = 0;

            StepState st = getStepState(stepIdx);
            float vel = stateToVelocity(st);
            double offsetMs = 0.0;

            // Apply training modules (per tick) to Grid A only
            const int modCount = m_trainingModuleCount.load(std::memory_order_acquire);
            if (modCount > 0) {
                ITrainingModule::Step step;
                step.stepIndex = static_cast<uint16_t>(stepIdx);
                step.stepsPerBar = static_cast<uint16_t>(stepsPerBar);
                step.isDownbeat = (stepIdx == 0);
                step.beatIndex = static_cast<uint16_t>(partsPerQuarter > 0 ? (stepIdx / partsPerQuarter) : 0);
                step.subIndex = static_cast<uint16_t>(partsPerQuarter > 0 ? (stepIdx % partsPerQuarter) : 0);
                step.isBeatStart = (step.subIndex == 0);
                step.state = static_cast<ITrainingModule::StepState>(static_cast<uint8_t>(st));
                step.velocity = vel;

                for (int mi = 0; mi < modCount; ++mi) {
                    ITrainingModule* m = m_trainingModules[mi];
                    if (!m) continue;
                    m->modifyNextStep(step, offsetMs);
                    const double ov = m->getBpmOverride();
                    if (ov > 0.0) {
                        m_bpm.store(std::clamp(ov, 10.0, 1000.0), std::memory_order_relaxed);
                        bpmNow = m_bpm.load(std::memory_order_relaxed);
                    }
                }

                st = static_cast<StepState>(static_cast<uint8_t>(step.state));
                vel = step.velocity;
            }
            // Recompute step lengths if bpmNow changed by overrides
            computeStepLens();

            // Apply offset (ms -> samples) to the click event moment.
            int64_t offsetSamples = 0;
            if (offsetMs != 0.0 && std::isfinite(offsetMs)) {
                offsetSamples = static_cast<int64_t>(std::llround(offsetMs * static_cast<double>(sr) / 1000.0));
            }
            int64_t eventFrame64 = static_cast<int64_t>(frame) + offsetSamples;
            if (eventFrame64 < 0) eventFrame64 = 0;
            if (eventFrame64 >= static_cast<int64_t>(nFrames)) eventFrame64 = static_cast<int64_t>(nFrames - 1);
            const uint32_t eventFrame = static_cast<uint32_t>(eventFrame64);

            if (written < outCapacity) {
                outEvents[written++] = Event{
                    eventFrame,
                    static_cast<uint16_t>(stepIdx),
                    GridId::A,
                    st,
                    vel
                };
            }

            // Downbeat sync: force GridB to align at this sample
            if (gridBEnabled && stepIdx == 0) {
                m_currentStepB.store(0, std::memory_order_relaxed);
                remainingB = 0.0;
            }

            int nextStep = m_currentStep.load(std::memory_order_relaxed) + 1;
            if (nextStep >= stepsPerBar) nextStep = 0;
            m_currentStep.store(nextStep, std::memory_order_relaxed);

            remainingA += stepLenA;
        }

        // Emit B if due
        if (gridBEnabled && remainingB <= 0.0) {
            int stepIdxB = m_currentStepB.load(std::memory_order_relaxed);
            if (stepIdxB < 0 || stepIdxB >= stepsPerBarB) stepIdxB = 0;

            StepState stB = getStepStateB(stepIdxB);
            float velB = stateToVelocity(stB);

            if (written < outCapacity) {
                outEvents[written++] = Event{
                    frame,
                    static_cast<uint16_t>(stepIdxB),
                    GridId::B,
                    stB,
                    velB
                };
            }

            int nextB = stepIdxB + 1;
            if (nextB >= stepsPerBarB) nextB = 0;
            m_currentStepB.store(nextB, std::memory_order_relaxed);
            remainingB += stepLenB;
        }
    }

    // Carry remainder into next block.
    // Keep it bounded to avoid runaway values (use 1 minute at 10 BPM as a very loose cap).
    if (!std::isfinite(remainingA)) remainingA = 0.0;
    const double hardCap = static_cast<double>(sr) * 60.0;
    if (remainingA < -hardCap) remainingA = -hardCap;
    if (remainingA > hardCap) remainingA = hardCap;
    m_samplesUntilNextTick = remainingA;

    if (!std::isfinite(remainingB)) remainingB = 0.0;
    if (remainingB < -hardCap) remainingB = -hardCap;
    if (remainingB > hardCap) remainingB = hardCap;
    m_samplesUntilNextTickB = remainingB;
    m_totalSamples += nFrames;

    return written;
}
