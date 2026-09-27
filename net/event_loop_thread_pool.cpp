#include "event_loop_thread_pool.h"
#include "event_loop.h"

#include <cstdio>

EventLoopThreadPool::EventLoopThreadPool(EventLoop *base_loop, int thread_num)
    : m_base_loop(base_loop), m_thread_num(thread_num), m_next(0), m_started(0), m_stopped(false)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    stop();
}

void EventLoopThreadPool::start()
{
    if (m_thread_num <= 0)
        return; //不建线程：全部连接归主循环

    m_loops.resize(m_thread_num, nullptr);
    for (int i = 0; i < m_thread_num; ++i)
        m_threads.emplace_back([this, i] { thread_func(i); });

    //等待所有循环就绪再返回：调用方拿到 next_loop() 的指针后不再同步
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cond.wait(lock, [this] { return m_started == m_thread_num; });
}

void EventLoopThreadPool::thread_func(int index)
{
    //循环建在线程栈上：它的析构发生在该线程内，于是连接与定时器的最终释放
    //也发生在正确的线程上——若由主线程持有并析构，就会在那里释放别人的资源
    EventLoop loop;

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_loops[index] = &loop;
        ++m_started;
    }
    m_cond.notify_one();

    loop.loop();

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_loops[index] = nullptr; //循环已退出，登记随之失效
    }
}

EventLoop *EventLoopThreadPool::next_loop()
{
    if (m_thread_num <= 0)
        return m_base_loop;

    const int index = m_next.fetch_add(1) % m_thread_num;
    return m_loops[index];
}

void EventLoopThreadPool::stop()
{
    if (m_stopped)
        return;

    m_stopped = true;

    //quit 是线程安全的：它置位后写 eventfd 把阻塞中的循环叫醒
    for (EventLoop *loop : m_loops)
    {
        if (loop != nullptr)
            loop->quit();
    }

    for (std::thread &thread : m_threads)
    {
        if (thread.joinable())
            thread.join();
    }
    m_threads.clear();
}
