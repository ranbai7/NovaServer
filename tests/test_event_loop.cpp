// EventLoop 与 Channel 的单元测试
//
// 这两者的行为只能在「真的跑起来」的前提下验证：事件分派、跨线程投递、
// 唤醒机制都是运行时的时序行为，没有可单独断言的纯函数。
#include "../net/channel.h"
#include "../net/event_loop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unistd.h>

namespace
{
//等待条件成立，超时返回 false。用于「另一个线程做完某事」这类同步，
//避免在测试里写死 sleep 时长
template <typename Predicate> bool wait_for(Predicate pred, int timeout_ms = 2000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}
} // namespace

TEST(EventLoopTest, RunInLoopExecutesImmediatelyOnOwnerThread)
{
    EventLoop loop;
    bool executed = false;

    //未调用 loop() 也算「本线程」：归属是按线程判定的，不是按循环是否在跑
    loop.run_in_loop([&executed] { executed = true; });

    EXPECT_TRUE(executed);
}

TEST(EventLoopTest, QueueInLoopFromOtherThreadWakesUpLoop)
{
    EventLoop loop;
    std::atomic<bool> executed{false};
    std::atomic<bool> loop_started{false};

    std::thread worker(
        [&loop, &executed, &loop_started]
        {
            //等主线程真正阻塞在 epoll_wait 之后再投递，否则任务是在循环启动前入队的，
            //测不到「唤醒」这条路径
            if (!wait_for([&loop_started] { return loop_started.load(); }))
                return;

            loop.queue_in_loop([&executed] { executed = true; });
            wait_for([&executed] { return executed.load(); });
            loop.quit();
        });

    loop_started = true;
    loop.loop(); //quit() 之后返回

    worker.join();
    EXPECT_TRUE(executed.load());
}

TEST(EventLoopTest, QuitFromOtherThreadStopsLoop)
{
    EventLoop loop;
    std::atomic<bool> loop_started{false};

    std::thread quitter(
        [&loop, &loop_started]
        {
            if (!wait_for([&loop_started] { return loop_started.load(); }))
                return;
            loop.quit();
        });

    loop_started = true;
    loop.loop(); //若 quit 的唤醒失效，这里会一直阻塞直到测试超时

    quitter.join();
    SUCCEED();
}

TEST(EventLoopTest, ChannelDispatchesReadEvent)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));

    EventLoop loop;
    Channel channel(&loop, fds[0]);
    std::atomic<int> reads{0};
    channel.set_read_callback(
        [&reads, fds]
        {
            char buf[16];
            const ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0)
                reads.fetch_add(1);
            //不在此处 quit：读走数据后管道不再可读，LT 下也不会重复触发
        });
    channel.enable_reading();

    //不必与循环启动同步：写在前则循环启动后立即读到，「写在后」则由唤醒路径送达
    std::thread writer(
        [&loop, &reads, fds]
        {
            const char payload = 'x';
            EXPECT_EQ(1, write(fds[1], &payload, 1));
            wait_for([&reads] { return reads.load() > 0; });
            loop.quit();
        });

    loop.loop();
    writer.join();

    EXPECT_EQ(1, reads.load());
    channel.remove();
    close(fds[0]);
    close(fds[1]);
}

TEST(EventLoopTest, ChannelDispatchesWriteEvent)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));

    EventLoop loop;
    Channel channel(&loop, fds[1]);
    std::atomic<bool> writable{false};
    channel.set_write_callback(
        [&writable, &loop]
        {
            writable = true;
            //必须在这里退出循环：管道可写是持续状态，LT 下会反复触发
            loop.quit();
        });
    channel.enable_writing();

    loop.loop();

    EXPECT_TRUE(writable.load());
    channel.remove();
    close(fds[0]);
    close(fds[1]);
}

TEST(ChannelTest, EventIsDroppedAfterOwnerDestroyed)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));

    EventLoop loop;
    auto owner = std::make_shared<int>(1);
    Channel channel(&loop, fds[0]);
    channel.tie(owner);

    int calls = 0;
    channel.set_read_callback([&calls] { ++calls; });

    owner.reset(); //持有者先于描述符销毁

    //tie 的作用是让这种情况被安静丢弃，而不是让回调操作一份已释放的对象
    channel.handle_event(EPOLLIN);
    EXPECT_EQ(0, calls);

    close(fds[0]);
    close(fds[1]);
}

TEST(ChannelTest, OwnerSurvivesCallbackThatReleasesIt)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));

    EventLoop loop;
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> weak = owner;

    Channel channel(&loop, fds[0]);
    channel.tie(owner);
    int calls = 0;
    channel.set_read_callback(
        [&owner, &calls]
        {
            ++calls;
            owner.reset(); //回调里释放最后一个强引用：guard 未析构前对象应仍然存活
        });

    channel.handle_event(EPOLLIN);

    EXPECT_EQ(1, calls);
    EXPECT_TRUE(weak.expired()); //回调结束后才真正释放

    close(fds[0]);
    close(fds[1]);
}

TEST(ChannelTest, EnableReadingDoesNotRepeatEpollCtl)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));

    EventLoop loop;
    Channel channel(&loop, fds[0]);
    channel.enable_reading();

    //重复请求同一关注事件不应改变状态，调用方据此跳过多余的 epoll_ctl
    const uint32_t before = channel.events();
    channel.enable_reading();
    EXPECT_EQ(before, channel.events());

    channel.enable_writing();
    EXPECT_TRUE(channel.is_writing());
    channel.disable_writing();
    EXPECT_FALSE(channel.is_writing());

    channel.remove();
    EXPECT_FALSE(channel.in_epoll());
    close(fds[0]);
    close(fds[1]);
}
