#pragma once
#include <vector>
#include <string>
#include <cstdint>

/**
 * @brief Tempo map for time-based BPM automation.
 *
 * Stores points like:
 *  - 00:00 -> 120 BPM
 *  - 02:40 -> 90 BPM
 *
 * Between points, BPM is linearly interpolated.
 */
class TempoMap {
public:
    struct Point {
        double timeSec = 0.0; // >= 0
        double bpm = 120.0;   // > 0
    };

    TempoMap() = default;

    /**
     * @brief Replace all points (sorted internally).
     */
    void setPoints(std::vector<Point> points);

    /**
     * @brief Parse points from text.
     *
     * Supported line formats:
     *  - "MM:SS BPM"        e.g. "02:40 90"
     *  - "HH:MM:SS BPM"     e.g. "01:02:03 128.5"
     *  - "SS BPM"           e.g. "160 90"
     * Separators: space or '=' (e.g. "02:40 = 90")
     * Lines starting with '#' are ignored.
     *
     * @return true if at least one valid point parsed.
     */
    bool parseFromText(const std::string& text);

    /**
     * @brief BPM at a given time (seconds), with linear interpolation.
     */
    double bpmAt(double timeSec) const;

    const std::vector<Point>& points() const { return m_points; }
    bool empty() const { return m_points.empty(); }

    /**
     * @brief Parse a timestamp into seconds (SS, MM:SS, HH:MM:SS).
     * @return true on success.
     */
    static bool parseTimestamp(const std::string& token, double& outSec);

private:
    std::vector<Point> m_points;
};

