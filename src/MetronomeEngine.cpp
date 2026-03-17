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
