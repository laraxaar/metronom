#include "MetronomeEngine.h"
#include <algorithm>
#include <cmath>

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
}

void MetronomeEngine::setSampleRate(uint32_t sampleRate) {
    if (sampleRate == 0) sampleRate = 48000;
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
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
}

void MetronomeEngine::setSubdivisionParts(int partsPerQuarter) {
    partsPerQuarter = coerceSubdivisionParts(partsPerQuarter);
    m_partsPerQuarter.store(partsPerQuarter, std::memory_order_relaxed);

    int beats = m_beatsPerBar.load(std::memory_order_relaxed);
    int newSteps = std::clamp(beats * partsPerQuarter, 1, MAX_STEPS);
    rebuildGridForNewSteps(newSteps);
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
}

size_t MetronomeEngine::processBlock(uint32_t nFrames, Event* outEvents, size_t outCapacity) {
    if (!outEvents || outCapacity == 0 || nFrames == 0) return 0;

    const uint32_t sr = m_sampleRate.load(std::memory_order_relaxed);
    const double bpm = m_bpm.load(std::memory_order_relaxed);
    const int stepsPerBar = m_stepsPerBar.load(std::memory_order_relaxed);
    const int partsPerQuarter = m_partsPerQuarter.load(std::memory_order_relaxed);

    if (sr == 0 || bpm <= 0.0 || stepsPerBar <= 0 || partsPerQuarter <= 0) return 0;

    // One quarter note duration in samples.
    const double samplesPerQuarter = (static_cast<double>(sr) * 60.0) / bpm;
    const double samplesPerStep = samplesPerQuarter / static_cast<double>(partsPerQuarter);

    // Guard against extreme values.
    const double stepLen = std::clamp(samplesPerStep, 1.0, 1.0e9);

    size_t written = 0;

    // We maintain "samples until next tick". Decrease it by each processed sample offset.
    // When it reaches <= 0, emit tick(s) and push it forward by stepLen.
    double remaining = m_samplesUntilNextTick;

    // If starting fresh or after reset, schedule immediate tick at offset 0.
    if (!std::isfinite(remaining)) remaining = 0.0;

    uint32_t frame = 0;
    while (frame < nFrames) {
        // Next tick inside this block?
        if (remaining > 0.0) {
            uint32_t advance = static_cast<uint32_t>(std::min<double>(remaining, static_cast<double>(nFrames - frame)));
            frame += advance;
            remaining -= static_cast<double>(advance);
            continue;
        }

        // Tick at current frame.
        if (written < outCapacity) {
            int step = m_currentStep.load(std::memory_order_relaxed);
            if (step < 0 || step >= stepsPerBar) step = 0;
            StepState st = getStepState(step);
            outEvents[written++] = Event{
                frame,
                static_cast<uint16_t>(step),
                st,
                stateToVelocity(st)
            };
        } else {
            // Capacity exceeded: still advance playhead to keep timing correct.
        }

        int nextStep = m_currentStep.load(std::memory_order_relaxed) + 1;
        if (nextStep >= stepsPerBar) nextStep = 0;
        m_currentStep.store(nextStep, std::memory_order_relaxed);

        remaining += stepLen;
    }

    // Carry remainder into next block.
    // Keep it in [0, stepLen) to avoid runaway drift.
    if (remaining < 0.0) {
        // If bpm changed aggressively, we might be "late"; bring it back by adding stepLen.
        remaining = std::fmod(remaining, stepLen);
        if (remaining < 0.0) remaining += stepLen;
    } else if (remaining >= stepLen) {
        remaining = std::fmod(remaining, stepLen);
    }
    m_samplesUntilNextTick = remaining;

    return written;
}

#include "MetronomeEngine.h"
#include <cmath>
#include <algorithm>

// =====================================================================
// Constructor
// =====================================================================

MetronomeEngine::MetronomeEngine() {
    // Initialize all steps to NORMAL
    for (int i = 0; i < MAX_STEPS; ++i) {
        m_steps[i].store(static_cast<uint8_t>(StepType::NORMAL), std::memory_order_relaxed);
    }
    // First step is always ACCENT by default
    m_steps[0].store(static_cast<uint8_t>(StepType::ACCENT), std::memory_order_relaxed);

    m_numSteps.store(4, std::memory_order_relaxed);
    m_samplesUntilNextTick = 0;
}

// =====================================================================
// Initialization
// =====================================================================

void MetronomeEngine::setSampleRate(uint32_t sampleRate) {
    m_sampleRate = sampleRate;
    m_samplesUntilNextTick = calcSamplesPerTick();
}

void MetronomeEngine::reset() {
    m_currentStep.store(0, std::memory_order_relaxed);
    m_samplesUntilNextTick = calcSamplesPerTick();
    m_totalSamplesProcessed = 0;
}

// =====================================================================
// Parameter Setters (thread-safe)
// =====================================================================

void MetronomeEngine::setBpm(double bpm) {
    if (bpm < 10.0) bpm = 10.0;
    if (bpm > 1000.0) bpm = 1000.0;
    m_bpm.store(bpm, std::memory_order_relaxed);
    // Note: samplesPerTick is recalculated live in processBlock
    // so there's no need to update m_samplesUntilNextTick here.
    // This allows truly seamless BPM changes without glitches.
}

void MetronomeEngine::setBeatsPerBar(int beats) {
    if (beats < 1) beats = 1;
    if (beats > 32) beats = 32;
    m_beatsPerBar.store(beats, std::memory_order_relaxed);
    rebuildGrid();
}

void MetronomeEngine::setSubdivision(int subdivision) {
    // Only allow supported subdivision values
    if (subdivision != 1 && subdivision != 2 && subdivision != 3 &&
        subdivision != 4 && subdivision != 5 && subdivision != 7) {
        subdivision = 1;
    }
    m_subdivision.store(subdivision, std::memory_order_relaxed);
    rebuildGrid();
}

// =====================================================================
// Grid Manipulation (thread-safe via atomic array)
// =====================================================================

void MetronomeEngine::cycleStep(int stepIndex) {
    int numSteps = m_numSteps.load(std::memory_order_relaxed);
    if (stepIndex < 0 || stepIndex >= numSteps) return;

    uint8_t current = m_steps[stepIndex].load(std::memory_order_relaxed);
    uint8_t next = (current + 1) % 3;
    m_steps[stepIndex].store(next, std::memory_order_relaxed);
}

void MetronomeEngine::setStep(int stepIndex, StepType type) {
    if (stepIndex >= 0 && stepIndex < MAX_STEPS) {
        m_steps[stepIndex].store(static_cast<uint8_t>(type), std::memory_order_relaxed);
    }
}

StepType MetronomeEngine::getStep(int stepIndex) const {
    if (stepIndex < 0 || stepIndex >= MAX_STEPS) return StepType::MUTE;
    return static_cast<StepType>(m_steps[stepIndex].load(std::memory_order_relaxed));
}

float MetronomeEngine::getVelocity(int stepIndex) const {
    StepType t = getStep(stepIndex);
    switch (t) {
        case StepType::ACCENT: return 1.0f;
        case StepType::NORMAL: return 0.6f;
        case StepType::MUTE:   return 0.0f;
    }
    return 0.0f;
}

// =====================================================================
// Audio Processing — sample-accurate tick generation
// =====================================================================

void MetronomeEngine::processBlock(uint32_t nFrames, std::vector<ClickEvent>& outEvents) {
    outEvents.clear();
    if (!m_running.load(std::memory_order_relaxed)) return;

    int numSteps = m_numSteps.load(std::memory_order_relaxed);
    if (numSteps <= 0) return;

    int subdivision = m_subdivision.load(std::memory_order_relaxed);
    int beatsPerBar = m_beatsPerBar.load(std::memory_order_relaxed);

    // Recalculate samples per tick based on current BPM + subdivision
    // samplesPerTick = sampleRate * 60 / (BPM * subdivision)
    // This is the interval between consecutive steps in the grid.
    uint64_t samplesPerTick = calcSamplesPerTick();

    for (uint32_t frame = 0; frame < nFrames; ++frame) {
        if (m_samplesUntilNextTick == 0) {
            int step = m_currentStep.load(std::memory_order_relaxed);

            // Build the click event
            ClickEvent evt;
            evt.sampleOffset = frame;
            evt.stepIndex = step;
            evt.beatIndex = (subdivision > 0) ? (step / subdivision) : step;
            evt.subIndex = (subdivision > 0) ? (step % subdivision) : 0;
            evt.type = getStep(step);
            evt.velocity = getVelocity(step);
            evt.isDownbeat = (step == 0);

            outEvents.push_back(evt);

            // Advance playhead
            int nextStep = (step + 1) % numSteps;
            m_currentStep.store(nextStep, std::memory_order_relaxed);

            // Reload samples per tick (BPM might have changed)
            samplesPerTick = calcSamplesPerTick();
            m_samplesUntilNextTick = samplesPerTick;
        }

        if (m_samplesUntilNextTick > 0) {
            --m_samplesUntilNextTick;
        }

        ++m_totalSamplesProcessed;
    }
}

// =====================================================================
// Snapshot
// =====================================================================

EngineSnapshot MetronomeEngine::getSnapshot() const {
    EngineSnapshot snap;
    int numSteps = m_numSteps.load(std::memory_order_relaxed);
    snap.numSteps = numSteps;
    snap.currentStep = m_currentStep.load(std::memory_order_relaxed);
    snap.beatsPerBar = m_beatsPerBar.load(std::memory_order_relaxed);
    snap.subdivision = m_subdivision.load(std::memory_order_relaxed);
    snap.bpm = m_bpm.load(std::memory_order_relaxed);
    snap.running = m_running.load(std::memory_order_relaxed);

    for (int i = 0; i < (std::min)(numSteps, MAX_STEPS); ++i) {
        snap.steps[i] = m_steps[i].load(std::memory_order_relaxed);
    }
    return snap;
}

// =====================================================================
// Internal Helpers
// =====================================================================

uint64_t MetronomeEngine::calcSamplesPerTick() const {
    double bpm = m_bpm.load(std::memory_order_relaxed);
    int subdivision = m_subdivision.load(std::memory_order_relaxed);

    if (bpm <= 0.0) bpm = 120.0;
    if (subdivision <= 0) subdivision = 1;

    // One quarter note = 60/BPM seconds
    // One subdivided tick = 60 / (BPM * subdivision) seconds
    // In samples = sampleRate * 60 / (BPM * subdivision)
    double samplesPerTickD = static_cast<double>(m_sampleRate) * 60.0 / (bpm * subdivision);

    uint64_t result = static_cast<uint64_t>(std::round(samplesPerTickD));
    if (result < 1) result = 1;
    return result;
}

void MetronomeEngine::rebuildGrid() {
    int beats = m_beatsPerBar.load(std::memory_order_relaxed);
    int sub = m_subdivision.load(std::memory_order_relaxed);
    int newNumSteps = beats * sub;

    if (newNumSteps > MAX_STEPS) newNumSteps = MAX_STEPS;
    if (newNumSteps < 1) newNumSteps = 1;

    int oldNumSteps = m_numSteps.load(std::memory_order_relaxed);

    // Initialize new steps: downbeats get ACCENT, others NORMAL
    for (int i = 0; i < newNumSteps; ++i) {
        if (i < oldNumSteps) {
            // Keep existing states for overlapping steps
            continue;
        }
        // New steps: first of each beat = ACCENT, rest = NORMAL
        if (sub > 0 && (i % sub) == 0) {
            m_steps[i].store(static_cast<uint8_t>(StepType::ACCENT), std::memory_order_relaxed);
        } else {
            m_steps[i].store(static_cast<uint8_t>(StepType::NORMAL), std::memory_order_relaxed);
        }
    }

    // Mute anything beyond the new grid
    for (int i = newNumSteps; i < MAX_STEPS; ++i) {
        m_steps[i].store(static_cast<uint8_t>(StepType::MUTE), std::memory_order_relaxed);
    }

    m_numSteps.store(newNumSteps, std::memory_order_relaxed);

    // Clamp playhead
    int curStep = m_currentStep.load(std::memory_order_relaxed);
    if (curStep >= newNumSteps) {
        m_currentStep.store(0, std::memory_order_relaxed);
    }
}
