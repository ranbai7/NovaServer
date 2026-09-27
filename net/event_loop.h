#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/epoll.h>
#include <thread>
#include <vector>

#include "channel.h"

//单线程事件循环：一个 epoll 实例、一个用于跨线程唤醒的 eventfd，以及一个待执行
//任务队列。归属约定是结构性的——所有事件处理、连接状态与超时定时器都只在本线程内
//访问，跨线程只能经 run_in_loop / queue_in_loop 投递任务。这条约定成立之后，
//「同一连接上读写串行」不需要任何附加机制
class EventLoop
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    //事件循环主体，必须由创建它的线程调用；quit() 之后返回
    void loop();
    //线程安全：置位后用 eventfd 把阻塞在 epoll_wait 的循环叫醒
    void quit();

    //在本线程则立即执行，否则入队并唤醒
    void run_in_loop(Functor cb);
    //只入队，由循环线程在下一轮取出执行
    void queue_in_loop(Functor cb);

    //依据 Channel 当前的关注事件决定 ADD 还是 MOD
    void update_channel(Channel *channel);
    void remove_channel(Channel *channel);

    bool is_in_loop_thread() const { return m_thread_id == std::this_thread::get_id(); }

private:
    void wakeup();
    void handle_wakeup();
    void do_pending_functors();

    static const int kMaxEvents = 10000;
    int m_epollfd;
    int m_wakeupfd; //eventfd：跨线程唤醒
    std::unique_ptr<Channel> m_wakeup_channel;
    std::atomic<bool> m_quitting;
    std::thread::id m_thread_id;
    std::mutex m_mutex;
    std::vector<Functor> m_pending_functors;
    std::vector<epoll_event> m_events;
};

#endif
