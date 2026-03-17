#pragma once
#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>

/**
 * @brief Lock-free SPSC Ring Buffer optimized for continuous audio sample streams.
 *
 * Uses power-of-two sizing for fast modular arithmetic (bitwise AND).
 * Producer (audio callback) writes blocks of samples via write().
 * Consumer (analysis thread) reads windows via read() or peek() without consuming.
 *
 * Thread-safety: Single-Producer Single-Consumer only.
 * No locks, no allocations in hot path.
 */
template <typename T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "RingBuffer requires trivially copyable types");

public:
    /**
     * @brief Construct a ring buffer with at least `minCapacity` elements.
     * Actual capacity is rounded up to the next power of two.
     */
    explicit RingBuffer(size_t minCapacity = 65536)
        : m_capacity(nextPowerOfTwo(minCapacity))
        , m_mask(m_capacity - 1)
        , m_writePos(0)
        , m_readPos(0)
    {
        m_buffer = new T[m_capacity]();
    }

    ~RingBuffer() {
        delete[] m_buffer;
    }

    // Non-copyable, movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    RingBuffer(RingBuffer&& other) noexcept
        : m_buffer(other.m_buffer)
        , m_capacity(other.m_capacity)
        , m_mask(other.m_mask)
        , m_writePos(other.m_writePos.load())
        , m_readPos(other.m_readPos.load())
    {
        other.m_buffer = nullptr;
        other.m_capacity = 0;
    }

    /**
     * @brief Write a block of samples into the buffer (producer side).
     * @param data Pointer to source samples.
     * @param count Number of samples to write.
     * @return Number of samples actually written (may be < count if buffer is full).
     */
    size_t write(const T* data, size_t count) {
        const size_t writePos = m_writePos.load(std::memory_order_relaxed);
        const size_t readPos  = m_readPos.load(std::memory_order_acquire);
        const size_t avail    = m_capacity - (writePos - readPos);
        const size_t toWrite  = (count < avail) ? count : avail;

        if (toWrite == 0) return 0;

        const size_t idx = writePos & m_mask;
        const size_t firstChunk = m_capacity - idx;

        if (toWrite <= firstChunk) {
            std::memcpy(m_buffer + idx, data, toWrite * sizeof(T));
        } else {
            std::memcpy(m_buffer + idx, data, firstChunk * sizeof(T));
            std::memcpy(m_buffer, data + firstChunk, (toWrite - firstChunk) * sizeof(T));
        }

        m_writePos.store(writePos + toWrite, std::memory_order_release);
        return toWrite;
    }

    /**
     * @brief Read and consume samples from the buffer (consumer side).
     * @param dest Destination buffer.
     * @param count Number of samples to read.
     * @return Number of samples actually read.
     */
    size_t read(T* dest, size_t count) {
        const size_t readPos  = m_readPos.load(std::memory_order_relaxed);
        const size_t writePos = m_writePos.load(std::memory_order_acquire);
        const size_t avail    = writePos - readPos;
        const size_t toRead   = (count < avail) ? count : avail;

        if (toRead == 0) return 0;

        const size_t idx = readPos & m_mask;
        const size_t firstChunk = m_capacity - idx;

        if (toRead <= firstChunk) {
            std::memcpy(dest, m_buffer + idx, toRead * sizeof(T));
        } else {
            std::memcpy(dest, m_buffer + idx, firstChunk * sizeof(T));
            std::memcpy(dest + firstChunk, m_buffer, (toRead - firstChunk) * sizeof(T));
        }

        m_readPos.store(readPos + toRead, std::memory_order_release);
        return toRead;
    }

    /**
     * @brief Peek at samples without consuming them (consumer side).
     * Reads `count` samples starting from `offset` samples ahead of the read position.
     * No data is consumed — the read pointer stays unchanged.
     * Ideal for overlapping analysis windows (YIN/MPM need this).
     *
     * @param dest Destination buffer.
     * @param count Number of samples to peek.
     * @param offset Offset from current read position (default 0).
     * @return Number of samples actually peeked.
     */
    size_t peek(T* dest, size_t count, size_t offset = 0) const {
        const size_t readPos  = m_readPos.load(std::memory_order_relaxed);
        const size_t writePos = m_writePos.load(std::memory_order_acquire);
        const size_t avail    = writePos - readPos;

        if (offset >= avail) return 0;

        const size_t actualAvail = avail - offset;
        const size_t toPeek = (count < actualAvail) ? count : actualAvail;

        if (toPeek == 0) return 0;

        const size_t startPos = readPos + offset;
        const size_t idx = startPos & m_mask;
        const size_t firstChunk = m_capacity - idx;

        if (toPeek <= firstChunk) {
            std::memcpy(dest, m_buffer + idx, toPeek * sizeof(T));
        } else {
            std::memcpy(dest, m_buffer + idx, firstChunk * sizeof(T));
            std::memcpy(dest + firstChunk, m_buffer, (toPeek - firstChunk) * sizeof(T));
        }

        return toPeek;
    }

    /**
     * @brief Advance the read pointer without reading data.
     * @param count Number of samples to skip.
     * @return Number of samples actually skipped.
     */
    size_t skip(size_t count) {
        const size_t readPos  = m_readPos.load(std::memory_order_relaxed);
        const size_t writePos = m_writePos.load(std::memory_order_acquire);
        const size_t avail    = writePos - readPos;
        const size_t toSkip   = (count < avail) ? count : avail;

        m_readPos.store(readPos + toSkip, std::memory_order_release);
        return toSkip;
    }

    /** @brief Number of samples available for reading. */
    size_t availableRead() const {
        const size_t writePos = m_writePos.load(std::memory_order_acquire);
        const size_t readPos  = m_readPos.load(std::memory_order_relaxed);
        return writePos - readPos;
    }

    /** @brief Number of samples that can be written before the buffer is full. */
    size_t availableWrite() const {
        const size_t writePos = m_writePos.load(std::memory_order_relaxed);
        const size_t readPos  = m_readPos.load(std::memory_order_acquire);
        return m_capacity - (writePos - readPos);
    }

    /** @brief Reset the buffer (not thread-safe, call only when stream is stopped). */
    void reset() {
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
    }

    /** @brief Get the buffer capacity. */
    size_t capacity() const { return m_capacity; }

private:
    static size_t nextPowerOfTwo(size_t v) {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    T*                  m_buffer;
    size_t              m_capacity;
    size_t              m_mask;
    std::atomic<size_t> m_writePos;
    std::atomic<size_t> m_readPos;
};
