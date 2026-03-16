#pragma once
#include <atomic>
#include <vector>
#include <optional>

/**
 * @brief A lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
 * Perfect for sending events between the GUI thread and the Audio thread.
 */
template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity)
        : m_buffer(capacity + 1)
        , m_head(0)
        , m_tail(0)
    {}

    /**
     * @brief Pushes an item into the queue.
     * @return true if successful, false if the queue is full.
     */
    bool push(const T& item) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t nextHead = (head + 1) % m_buffer.size();

        if (nextHead == m_tail.load(std::memory_order_acquire)) {
            return false; // Queue is full
        }

        m_buffer[head] = item;
        m_head.store(nextHead, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pops an item from the queue.
     * @return The item if available, std::nullopt otherwise.
     */
    std::optional<T> pop() {
        size_t tail = m_tail.load(std::memory_order_relaxed);

        if (tail == m_head.load(std::memory_order_acquire)) {
            return std::nullopt; // Queue is empty
        }

        T item = m_buffer[tail];
        m_tail.store((tail + 1) % m_buffer.size(), std::memory_order_release);
        return item;
    }

    /**
     * @brief Check if the queue is empty.
     */
    bool isEmpty() const {
        return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
    }

private:
    std::vector<T> m_buffer;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};
