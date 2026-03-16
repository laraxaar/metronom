#include "UIController.h"

UIController::UIController(size_t queueCapacity)
    : m_visualQueue(queueCapacity)
{}

bool UIController::postVisualData(const VisualData& data) {
    return m_visualQueue.push(data);
}

std::optional<VisualData> UIController::getLatestVisualData() {
    return m_visualQueue.pop();
}
