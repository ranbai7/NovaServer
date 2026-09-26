// 同步原语单元测试
//
// 覆盖互斥锁的互斥语义与信号量的等待/唤醒语义。
#include "../lock/locker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

TEST(Locker, ProvidesMutualExclusion)
{
    locker mutex;
    long long counter = 0;
    const int kPerThread = 20000;

    auto worker = [&mutex, &counter] {
        for (int i = 0; i < kPerThread; ++i)
        {
            mutex.lock();
            ++counter;
            mutex.unlock();
        }
    };

    std::thread first(worker);
    std::thread second(worker);
    first.join();
    second.join();

    EXPECT_EQ(counter, 2 * kPerThread);
}

TEST(Locker, ExposesUnderlyingMutex)
{
    locker mutex;
    EXPECT_NE(mutex.get(), nullptr);
}

TEST(Sem, BlocksUntilPosted)
{
    sem semaphore(0);
    std::atomic<bool> released{false};

    std::thread waiter([&semaphore, &released] {
        semaphore.wait();
        released = true;
    });

    // 尚未 post，等待方应当仍处于阻塞状态
    EXPECT_FALSE(released.load());

    semaphore.post();
    waiter.join();

    EXPECT_TRUE(released.load());
}

TEST(Sem, HonorsInitialCount)
{
    sem semaphore(2);
    EXPECT_TRUE(semaphore.wait());
    EXPECT_TRUE(semaphore.wait());
}
