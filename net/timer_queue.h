#ifndef TIMER_QUEUE_H
#define TIMER_QUEUE_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "channel.h"

class EventLoop;

//基于 timerfd 的定时器队列。每个事件循环持有一份，定时器因此天然归所属线程所有——`ROADMAP` 里「定时器下沉至
//各子线程」即指此事，它让定时器与连接之间的并发访问在结构上不再存在。
//取消防御由调用方负责：回调通常持有所在对象的弱引用，对象先销毁时回调取到空指针直接丢弃；队列本身不关心
class TimerQueue
{
public:
    using TimerCallback = std::function<void()>;

    explicit TimerQueue(EventLoop *loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue &) = delete;
    TimerQueue &operator=(const TimerQueue &) = delete;

    //注册一个一次性定时器，返回标识；回调在本线程的事件循环中执行
    uint64_t add_timer(uint64_t delay_ms, TimerCallback cb);
    //顺延已有定时器。标识不存在时无副作用，因此调用方不必先判断对象是否还在
    void refresh_timer(uint64_t id, uint64_t delay_ms);
    void cancel_timer(uint64_t id);

private:
    struct Entry
    {
        std::chrono::steady_clock::time_point expire;
        TimerCallback cb;
    };

    void handle_read();
    //把 timerfd 重新装载到「最早的那个到期时刻」
    void rearm();
    static std::chrono::steady_clock::time_point earliest_of(const std::unordered_map<uint64_t, Entry> &entries);

    int m_timerfd;
    std::unique_ptr<Channel> m_channel;
    //不维护排序索引：条数是本线程的连接数，且只在该线程确实有定时器到期时才扫描一次（rearm 已把 timerfd 精确装到最早到期时刻）；换来不必同时维护「有序容器 + 反向索引」两份结构
    std::unordered_map<uint64_t, Entry> m_entries;
    uint64_t m_next_id;
    //当前已装载的到期时刻，用于跳过多余的 timerfd_settime
    std::chrono::steady_clock::time_point m_armed;
};

#endif
