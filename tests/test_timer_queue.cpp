// TimerQueue 与 SignalWatcher 的单元测试
//
// 两者都把「外部事件」转成了事件循环上的可读事件：定时器来自 timerfd、信号来自
// signalfd。验证方式只能是让循环真的跑起来，观察回调是否在预期的时刻发生
#include "../net/event_loop.h"
#include "../net/signal_watcher.h"
#include "../net/timer_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>

using namespace std::chrono;

TEST(TimerQueueTest, FiresAfterDelay)
{
    EventLoop loop;
    TimerQueue timers(&loop);
    std::atomic<int> fired{0};

    timers.add_timer(20,
                     [&fired, &loop]
                     {
                         fired.fetch_add(1);
                         loop.quit();
                     });

    loop.loop();

    EXPECT_EQ(1, fired.load());
}

TEST(TimerQueueTest, RefreshPostponesFiring)
{
    EventLoop loop;
    TimerQueue timers(&loop);
    milliseconds elapsed{0};
    const auto start = steady_clock::now();

    const uint64_t id = timers.add_timer(20,
                                         [&elapsed, &start, &loop]
                                         {
                                             elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
                                             loop.quit();
                                         });

    //在它到期之前顺延：原定 20ms，顺延到约 5+80ms
    timers.add_timer(5, [&timers, id] { timers.refresh_timer(id, 80); });

    loop.loop();

    //阈值取 60ms：既排除了「顺延未生效」（那样会在 20ms 附近触发），
    //也留出了调度延迟的余量
    EXPECT_GE(elapsed.count(), 60);
}

TEST(TimerQueueTest, CancelledTimerDoesNotFire)
{
    EventLoop loop;
    TimerQueue timers(&loop);
    std::atomic<bool> fired{false};

    const uint64_t id = timers.add_timer(10, [&fired] { fired = true; });
    timers.cancel_timer(id);

    //用一个稍晚的定时器结束循环，给被取消的那个留出本该触发的窗口
    timers.add_timer(40, [&loop] { loop.quit(); });

    loop.loop();

    EXPECT_FALSE(fired.load());
}

TEST(TimerQueueTest, CallbackCanCancelItself)
{
    EventLoop loop;
    TimerQueue timers(&loop);
    std::atomic<int> fired{0};

    //到期的定时器在执行回调前已从表中摘除，因此回调里再取消自己应当无副作用
    //（连接因空闲超时关闭时走的就是这条路：关闭动作会取消自己那个定时器）
    uint64_t id = 0;
    id = timers.add_timer(10,
                          [&fired, &timers, &id]
                          {
                              fired.fetch_add(1);
                              timers.cancel_timer(id);
                          });
    timers.add_timer(40, [&loop] { loop.quit(); });

    loop.loop();

    EXPECT_EQ(1, fired.load());
}

TEST(TimerQueueTest, RearmKeepsEarlierTimerOnRefresh)
{
    EventLoop loop;
    TimerQueue timers(&loop);
    std::atomic<int> order{0};
    int first_at = 0;
    int second_at = 0;

    //两个定时器：先到的那个被顺延后，仍应晚于原定的后一个触发
    const uint64_t early = timers.add_timer(15, [&] { first_at = ++order; });
    timers.add_timer(50, [&second_at, &order] { second_at = ++order; });

    timers.add_timer(5, [&timers, early] { timers.refresh_timer(early, 100); });
    timers.add_timer(150, [&loop] { loop.quit(); });

    loop.loop();

    //顺延后的那个最后触发：second 先，first 后
    EXPECT_LT(second_at, first_at);
}

TEST(SignalWatcherTest, DeliversBlockedSignal)
{
    //屏蔽必须在创建线程之前完成；本测试不创建线程，顺序上没有额外约束
    ASSERT_TRUE(SignalWatcher::block_signals({SIGUSR1}));

    EventLoop loop;
    SignalWatcher watcher(&loop, {SIGUSR1});
    std::atomic<int> received{0};
    std::atomic<int> last_signo{0};

    watcher.set_callback(
        [&received, &last_signo, &loop](int signo)
        {
            last_signo = signo;
            received.fetch_add(1);
            loop.quit();
        });

    ASSERT_EQ(0, raise(SIGUSR1));

    loop.loop();

    EXPECT_EQ(1, received.load());
    EXPECT_EQ(SIGUSR1, last_signo.load());
}
