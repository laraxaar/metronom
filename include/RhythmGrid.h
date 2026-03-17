#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>

/**
 * @brief State of a single step in the rhythm grid.
 */
enum class StepState : uint8_t {
    Accent  = 0,  // Velocity 1.0 — loud click
    Normal  = 1,  // Velocity 0.6 — regular click
    Muted   = 2   // Velocity 0.0 — silence
};

/**
 * @brief Subdivision types for the rhythm grid.
 */
enum class Subdivision : int {
    Quarter    = 4,
    Eighth     = 8,
    Triplet    = 12,
    Sixteenth  = 16,
    Quintuplet = 20
};

/**
 * @brief Snapshot of grid state for lock-free transfer to UI thread.
 */
struct GridSnapshot {
    uint8_t states[32]{};   // StepState cast to uint8_t
    int numSteps = 4;
    int currentStep = 0;    // Playhead position
    int subdivision = 4;
    int beatsPerBar = 4;
};

/**
 * @brief Manages the rhythm grid: per-step states, subdivisions, velocity mapping.
 *
 * The grid divides one bar into N steps based on time signature and subdivision.
 * Each step has a state (Accent/Normal/Muted) that maps to a velocity value.
 * 
 * Steps per bar = beatsPerBar * (subdivision / 4)
 * Example: 4/4 with Sixteenth = 4 * 4 = 16 steps
 */
class RhythmGrid {
public:
    static constexpr int MAX_STEPS = 32;

    RhythmGrid() {
        reset(4, Subdivision::Quarter);
    }

    /**
     * @brief Reset the grid with new time signature and subdivision.
     * Sets beat 0 to Accent, all others to Normal.
     */
    void reset(int beatsPerBar, Subdivision subdiv) {
        m_beatsPerBar = beatsPerBar;
        m_subdivision = subdiv;
        m_numSteps = calcNumSteps(beatsPerBar, subdiv);
        if (m_numSteps > MAX_STEPS) m_numSteps = MAX_STEPS;

        // Default pattern: first step Accent, downbeats Normal, rest Normal
        for (int i = 0; i < MAX_STEPS; ++i) {
            if (i >= m_numSteps) {
                m_steps[i] = StepState::Muted;
            } else if (i == 0) {
                m_steps[i] = StepState::Accent;
            } else {
                m_steps[i] = StepState::Normal;
            }
        }
        m_currentStep = 0;
    }

    /**
     * @brief Change subdivision, remapping existing states intelligently.
     */
    void setSubdivision(Subdivision subdiv) {
        int oldNumSteps = m_numSteps;
        int newNumSteps = calcNumSteps(m_beatsPerBar, subdiv);
        if (newNumSteps > MAX_STEPS) newNumSteps = MAX_STEPS;

        // Save old states
        StepState oldSteps[MAX_STEPS];
        for (int i = 0; i < MAX_STEPS; ++i) oldSteps[i] = m_steps[i];

        // Remap: scale indices proportionally
        for (int i = 0; i < newNumSteps; ++i) {
            int srcIdx = (i * oldNumSteps) / newNumSteps;
            if (srcIdx >= oldNumSteps) srcIdx = oldNumSteps - 1;
            m_steps[i] = oldSteps[srcIdx];
        }
        for (int i = newNumSteps; i < MAX_STEPS; ++i) {
            m_steps[i] = StepState::Muted;
        }

        m_numSteps = newNumSteps;
        m_subdivision = subdiv;
        if (m_currentStep >= m_numSteps) m_currentStep = 0;
    }

    void setBeatsPerBar(int beats) {
        reset(beats, m_subdivision);
    }

    /**
     * @brief Cycle a step through Accent → Normal → Muted → Accent.
     */
    void cycleStep(int index) {
        if (index < 0 || index >= m_numSteps) return;
        int s = static_cast<int>(m_steps[index]);
        m_steps[index] = static_cast<StepState>((s + 1) % 3);
    }

    void setStepState(int index, StepState state) {
        if (index >= 0 && index < m_numSteps) m_steps[index] = state;
    }

    StepState getStepState(int index) const {
        if (index < 0 || index >= m_numSteps) return StepState::Muted;
        return m_steps[index];
    }

    /**
     * @brief Get the velocity for a step (1.0 = Accent, 0.6 = Normal, 0.0 = Muted).
     */
    float getVelocity(int stepIndex) const {
        if (stepIndex < 0 || stepIndex >= m_numSteps) return 0.0f;
        switch (m_steps[stepIndex]) {
            case StepState::Accent: return 1.0f;
            case StepState::Normal: return 0.6f;
            case StepState::Muted:  return 0.0f;
        }
        return 0.0f;
    }

    /**
     * @brief Advance playhead. Called when a tick occurs.
     */
    void advanceStep() {
        m_currentStep = (m_currentStep + 1) % m_numSteps;
    }

    // Getters
    int getNumSteps() const { return m_numSteps; }
    int getCurrentStep() const { return m_currentStep; }
    void setCurrentStep(int s) { m_currentStep = s % m_numSteps; }
    int getBeatsPerBar() const { return m_beatsPerBar; }
    Subdivision getSubdivision() const { return m_subdivision; }

    /**
     * @brief Get a thread-safe snapshot for UI rendering.
     */
    GridSnapshot getSnapshot() const {
        GridSnapshot snap;
        for (int i = 0; i < MAX_STEPS; ++i) {
            snap.states[i] = static_cast<uint8_t>(m_steps[i]);
        }
        snap.numSteps = m_numSteps;
        snap.currentStep = m_currentStep;
        snap.subdivision = static_cast<int>(m_subdivision);
        snap.beatsPerBar = m_beatsPerBar;
        return snap;
    }

private:
    static int calcNumSteps(int beatsPerBar, Subdivision subdiv) {
        return beatsPerBar * (static_cast<int>(subdiv) / 4);
    }

    StepState m_steps[MAX_STEPS]{};
    int m_numSteps = 4;
    int m_currentStep = 0;
    int m_beatsPerBar = 4;
    Subdivision m_subdivision = Subdivision::Quarter;
};
