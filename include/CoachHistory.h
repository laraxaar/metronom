#pragma once
#include <string>
#include <array>
#include <cstdint>

/**
 * @brief Simple weekly history storage for coaching graphs.
 *
 * Stores last 7 days of aggregated stats.
 * Persistence: text file with one line per day.
 */
class CoachHistory {
public:
    struct DayStat {
        char date[11]{};   // "YYYY-MM-DD"
        float avgAccuracy = 0.0f;
        float avgStability = 0.0f;
        float avgPlayerBpm = 0.0f;
        float minutes = 0.0f;
    };

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    void pushOrUpdateToday(const DayStat& stat);

    const std::array<DayStat, 7>& days() const { return m_days; }

private:
    std::array<DayStat, 7> m_days{};
    void shiftLeft();
    static bool sameDay(const DayStat& a, const DayStat& b);
};

