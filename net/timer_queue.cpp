#include "timer_queue.h"
#include "event_loop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace
{
const std::chrono::steady_clock::time_point kNever = std::chrono::steady_clock::time_point::max();

timespec to_timespec(uint64_t delay_ms)
{
    timespec ts;
    ts.tv_sec = static_cast<time_t>(delay_ms / 1000);
    ts.tv_nsec = static_cast<long>((delay_ms % 1000) * 1000000);
    return ts;
}
} // namespace

TimerQueue::TimerQueue(EventLoop *loop) : m_timerfd(-1), m_next_id(1), m_armed(kNever)
{
    //CLOCK_MONOTONIC：超时判定必须用单调时钟，墙上时钟跳变不该让连接被误回收
    m_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (m_timerfd < 0)
    {
        std::perror("timerfd_create");
        std::exit(EXIT_FAILURE);
    }

    m_channel.reset(new Channel(loop, m_timerfd));
    m_channel->set_read_callback([this] { handle_read(); });
    m_channel->enable_reading();
}

TimerQueue::~TimerQueue()
{
    m_channel->remove();
    close(m_timerfd);
}

std::chrono::steady_clock::time_point TimerQueue::earliest_of(const std::unordered_map<uint64_t, Entry> &entries)
{
    auto earliest = kNever;
    for (const auto &pair : entries)
    {
        if (pair.second.expire < earliest)
            earliest = pair.second.expire;
    }
    return earliest;
}

uint64_t TimerQueue::add_timer(uint64_t delay_ms, TimerCallback cb)
{
    const uint64_t id = m_next_id++;
    m_entries[id] = Entry{std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms), std::move(cb)};
    rearm();
    return id;
}

void TimerQueue::refresh_timer(uint64_t id, uint64_t delay_ms)
{
    const auto it = m_entries.find(id);
    if (it == m_entries.end())
        return; //已被取消或对象已销毁，顺延请求自然作废

    it->second.expire = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    rearm();
}

void TimerQueue::cancel_timer(uint64_t id)
{
    if (0 == m_entries.erase(id))
        return;

    rearm();
}

void TimerQueue::handle_read()
{
    uint64_t expirations = 0;
    while (read(m_timerfd, &expirations, sizeof(expirations)) > 0)
    {
        //读一次即清零，循环是为了把可能累积的多次计数一并取走
    }

    const auto now = std::chrono::steady_clock::now();

    //先把到期项摘出来，再执行回调：回调里可能取消或顺延同一个定时器
    //（连接因空闲超时关闭时会取消自己），不先摘就会二次执行
    std::vector<TimerCallback> due;
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (it->second.expire <= now)
        {
            due.push_back(std::move(it->second.cb));
            it = m_entries.erase(it);
        }
        else
        {
            ++it;
        }
    }

    //timerfd 已到期，装载状态随之作废，需要按剩余定时器重新装载
    m_armed = kNever;
    rearm();

    for (const TimerCallback &cb : due)
        cb();
}

void TimerQueue::rearm()
{
    const auto earliest = earliest_of(m_entries);

    //装载时刻没有变化就不发起系统调用。这条判断不是可有可无的优化：
    //空闲超时在每次读写后都会 refresh，而 refresh 出来的到期时刻总是最晚的，
    //改不到最早时刻，因此绝大多数 refresh 都不该产生 timerfd_settime
    if (earliest == m_armed)
        return;

    itimerspec spec{};
    if (earliest != kNever)
    {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(earliest - std::chrono::steady_clock::now());
        //已经过期的按 1ms 处理：尽快触发，而不是传一个非法的负值
        spec.it_value = to_timespec(remaining.count() > 0 ? static_cast<uint64_t>(remaining.count()) : 1);
    }
    //earliest 为 kNever 时 it_value 保持全零，表示停止计时

    if (timerfd_settime(m_timerfd, 0, &spec, nullptr) < 0)
        std::perror("timerfd_settime");

    m_armed = earliest;
}
