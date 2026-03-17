#pragma once
#include <cstdint>

/**
 * @brief Training module interface for per-tick step modifications.
 *
 * Called from the audio callback on every tick. Must be real-time safe:
 * - no allocations
 * - no locks
 * - no I/O
 */
class ITrainingModule {
public:
    enum class StepState : uint8_t { ACCENT = 0, NORMAL = 1, MUTE = 2 };

    struct Step {
        uint16_t stepIndex = 0;   ///< [0..stepsPerBar-1]
        uint16_t stepsPerBar = 0;
        bool isDownbeat = false;  ///< true for stepIndex == 0
        uint16_t beatIndex = 0;   ///< beat index within bar (quarter-beat), if available
        uint16_t subIndex = 0;    ///< subdivision index within beat, if available
        bool isBeatStart = false; ///< subIndex == 0
        StepState state = StepState::NORMAL;
        float velocity = 0.6f;    ///< 1.0 / 0.6 / 0.0
    };

    virtual ~ITrainingModule() = default;

    /**
     * @brief Modify the upcoming tick.
     *
     * @param step In/out. Module may change state/velocity.
     * @param offsetMs In/out. Module may add micro timing offset in milliseconds.
     *
     * Notes:
     * - offsetMs is applied to the click trigger moment (positive = late).
     * - offsetMs does NOT change the base grid; it only shifts the click event.
     */
    virtual void modifyNextStep(Step& step, double& offsetMs) = 0;

    /**
     * @brief Optional tempo override (e.g. BPM Ladder).
     * @return new BPM to use, or <= 0.0 for "no override".
     */
    virtual double getBpmOverride() const { return -1.0; }
};

