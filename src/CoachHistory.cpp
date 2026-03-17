#include "CoachHistory.h"
#include <fstream>
#include <sstream>
#include <cstring>

static bool parseLine(const std::string& line, CoachHistory::DayStat& out) {
    // CSV: YYYY-MM-DD,acc,stab,bpm,minutes
    std::istringstream ss(line);
    std::string tok;
    if (!std::getline(ss, tok, ',')) return false;
    if (tok.size() != 10) return false;
    std::memset(out.date, 0, sizeof(out.date));
    std::memcpy(out.date, tok.c_str(), 10);

    auto readF = [&](float& v) -> bool {
        if (!std::getline(ss, tok, ',')) return false;
        try { v = std::stof(tok); } catch (...) { return false; }
        return true;
    };
    if (!readF(out.avgAccuracy)) return false;
    if (!readF(out.avgStability)) return false;
    if (!readF(out.avgPlayerBpm)) return false;
    if (!readF(out.minutes)) return false;
    return true;
}

bool CoachHistory::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    int idx = 0;
    while (std::getline(f, line) && idx < 7) {
        DayStat ds{};
        if (!parseLine(line, ds)) continue;
        m_days[idx++] = ds;
    }
    return true;
}

bool CoachHistory::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    for (const auto& d : m_days) {
        if (d.date[0] == '\0') continue;
        f << d.date << ","
          << d.avgAccuracy << ","
          << d.avgStability << ","
          << d.avgPlayerBpm << ","
          << d.minutes << "\n";
    }
    return true;
}

bool CoachHistory::sameDay(const DayStat& a, const DayStat& b) {
    return std::strncmp(a.date, b.date, 10) == 0;
}

void CoachHistory::shiftLeft() {
    for (int i = 1; i < 7; ++i) m_days[i - 1] = m_days[i];
    m_days[6] = DayStat{};
}

void CoachHistory::pushOrUpdateToday(const DayStat& stat) {
    if (stat.date[0] == '\0') return;
    // update if exists
    for (auto& d : m_days) {
        if (d.date[0] != '\0' && sameDay(d, stat)) {
            d = stat;
            return;
        }
    }
    // append to first empty, else shift and put at end
    for (auto& d : m_days) {
        if (d.date[0] == '\0') {
            d = stat;
            return;
        }
    }
    shiftLeft();
    m_days[6] = stat;
}

