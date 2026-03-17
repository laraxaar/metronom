#include "TempoMap.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

static std::string trimCopy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static bool parseDouble(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && end != s.c_str() && std::isfinite(out);
}

void TempoMap::setPoints(std::vector<Point> points) {
    // sanitize + sort
    for (auto& p : points) {
        if (!std::isfinite(p.timeSec) || p.timeSec < 0.0) p.timeSec = 0.0;
        if (!std::isfinite(p.bpm) || p.bpm <= 0.0) p.bpm = 120.0;
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.timeSec < b.timeSec;
    });

    // If duplicate timestamps exist, keep the last one (stable overwrite)
    std::vector<Point> dedup;
    dedup.reserve(points.size());
    for (const auto& p : points) {
        if (!dedup.empty() && std::abs(dedup.back().timeSec - p.timeSec) < 1e-9) {
            dedup.back() = p;
        } else {
            dedup.push_back(p);
        }
    }
    m_points = std::move(dedup);
}

bool TempoMap::parseTimestamp(const std::string& token, double& outSec) {
    const std::string t = trimCopy(token);
    if (t.empty()) return false;

    // Fast path: seconds as number
    {
        double v = 0.0;
        if (parseDouble(t, v) && v >= 0.0) {
            outSec = v;
            return true;
        }
    }

    // MM:SS or HH:MM:SS
    int parts[3] = {0, 0, 0};
    int count = 0;
    int acc = 0;
    bool haveDigit = false;

    auto flush = [&]() -> bool {
        if (!haveDigit) return false;
        if (count >= 3) return false;
        parts[count++] = acc;
        acc = 0;
        haveDigit = false;
        return true;
    };

    for (char ch : t) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            haveDigit = true;
            acc = acc * 10 + (ch - '0');
            if (acc > 999999) return false;
        } else if (ch == ':') {
            if (!flush()) return false;
        } else {
            return false;
        }
    }
    if (!flush()) return false;

    if (count == 2) {
        int mm = parts[0], ss = parts[1];
        if (ss < 0 || ss >= 60 || mm < 0) return false;
        outSec = static_cast<double>(mm) * 60.0 + static_cast<double>(ss);
        return true;
    }
    if (count == 3) {
        int hh = parts[0], mm = parts[1], ss = parts[2];
        if (ss < 0 || ss >= 60 || mm < 0 || mm >= 60 || hh < 0) return false;
        outSec = static_cast<double>(hh) * 3600.0 + static_cast<double>(mm) * 60.0 + static_cast<double>(ss);
        return true;
    }

    return false;
}

bool TempoMap::parseFromText(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::vector<Point> parsed;

    while (std::getline(in, line)) {
        line = trimCopy(line);
        if (line.empty()) continue;
        if (!line.empty() && line[0] == '#') continue;

        // Replace '=' with space for flexibility
        for (char& c : line) {
            if (c == '=') c = ' ';
        }

        std::istringstream ls(line);
        std::string tsTok;
        std::string bpmTok;
        if (!(ls >> tsTok)) continue;
        if (!(ls >> bpmTok)) continue;

        double sec = 0.0;
        if (!parseTimestamp(tsTok, sec)) continue;

        double bpm = 0.0;
        if (!parseDouble(bpmTok, bpm)) continue;
        if (bpm <= 0.0) continue;

        parsed.push_back({sec, bpm});
    }

    if (parsed.empty()) return false;
    setPoints(std::move(parsed));
    return !m_points.empty();
}

double TempoMap::bpmAt(double timeSec) const {
    if (m_points.empty()) return 120.0;
    if (!std::isfinite(timeSec) || timeSec <= m_points.front().timeSec) return m_points.front().bpm;
    if (timeSec >= m_points.back().timeSec) return m_points.back().bpm;

    auto it = std::upper_bound(
        m_points.begin(), m_points.end(), timeSec,
        [](double t, const Point& p) { return t < p.timeSec; }
    );

    // timeSec is between prev and it
    const Point& b = *it;
    const Point& a = *(it - 1);
    const double dt = b.timeSec - a.timeSec;
    if (dt <= 1e-12) return b.bpm;
    const double x = (timeSec - a.timeSec) / dt;
    return a.bpm + (b.bpm - a.bpm) * x;
}

