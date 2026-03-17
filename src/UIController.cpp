#include "UIController.h"

UIController::UIController(size_t queueCapacity)
    : m_visualQueue(queueCapacity)
{}

bool UIController::postVisualData(const VisualData& data) {
    return m_visualQueue.push(data);
}

std::optional<VisualData> UIController::getLatestVisualData() {
    std::optional<VisualData> latest = std::nullopt;
    while (auto opt = m_visualQueue.pop()) {
        latest = opt;
    }
    return latest;
}
