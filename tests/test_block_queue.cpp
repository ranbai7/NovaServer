// block_queue 单元测试
//
// 覆盖环形缓冲的基本操作、边界条件、超时行为与并发读写的正确性。
#include "../log/block_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
const int kQueueCapacity = 4;
const int kTimeoutMs = 50;
} // namespace

TEST(BlockQueue, InitiallyEmpty)
{
    block_queue<int> queue(kQueueCapacity);

    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.full());
    EXPECT_EQ(queue.size(), 0);
    EXPECT_EQ(queue.max_size(), kQueueCapacity);
}

TEST(BlockQueue, PushAndPopInOrder)
{
    block_queue<int> queue(kQueueCapacity);

    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_EQ(queue.size(), 2);

    int value = 0;
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(queue.empty());
}

TEST(BlockQueue, FrontAndBackTrackBothEnds)
{
    block_queue<int> queue(kQueueCapacity);
    queue.push(10);
    queue.push(20);
    queue.push(30);

    int value = 0;
    EXPECT_TRUE(queue.front(value));
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(queue.back(value));
    EXPECT_EQ(value, 30);
}

TEST(BlockQueue, RejectsPushWhenFull)
{
    block_queue<int> queue(2);
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.full());

    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.size(), 2);
}

TEST(BlockQueue, WrapsAroundCircularBuffer)
{
    // 反复写满再读空，覆盖环形下标的回绕边界
    block_queue<int> queue(3);
    int value = 0;

    for (int round = 0; round < 5; ++round)
    {
        for (int i = 0; i < 3; ++i)
            EXPECT_TRUE(queue.push(round * 10 + i));

        for (int i = 0; i < 3; ++i)
        {
            EXPECT_TRUE(queue.pop(value));
            EXPECT_EQ(value, round * 10 + i);
        }
    }
}

TEST(BlockQueue, ReturnsFalseWhenEmpty)
{
    block_queue<int> queue(kQueueCapacity);
    int value = 0;

    EXPECT_FALSE(queue.front(value));
    EXPECT_FALSE(queue.back(value));
}

TEST(BlockQueue, TimedPopGivesUpOnEmptyQueue)
{
    block_queue<int> queue(kQueueCapacity);
    int value = 0;

    // 队列为空时应在超时后返回，而不是一直阻塞
    EXPECT_FALSE(queue.pop(value, kTimeoutMs));
}

TEST(BlockQueue, TimedPopSucceedsWhenDataArrives)
{
    block_queue<int> queue(kQueueCapacity);
    int value = 0;

    std::thread producer([&queue] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        queue.push(42);
    });

    EXPECT_TRUE(queue.pop(value, 2000));
    EXPECT_EQ(value, 42);

    producer.join();
}

TEST(BlockQueue, ConcurrentProducerConsumerKeepsAllElements)
{
    const int kTotal = 2000;
    block_queue<int> queue(64);
    std::atomic<long long> sum{0};

    std::thread producer([&queue] {
        for (int i = 1; i <= kTotal; ++i)
        {
            while (!queue.push(i))
                std::this_thread::yield();
        }
    });

    std::thread consumer([&queue, &sum] {
        int value = 0;
        for (int i = 0; i < kTotal; ++i)
        {
            if (!queue.pop(value))
                return;
            sum += value;
        }
    });

    producer.join();
    consumer.join();

    const long long expected = static_cast<long long>(kTotal) * (kTotal + 1) / 2;
    EXPECT_EQ(sum.load(), expected);
}
