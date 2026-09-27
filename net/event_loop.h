#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/epoll.h>
#include <thread>
#include <unordered_set>
#include <vector>

#include "channel.h"
#include "timer_queue.h"

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

    //连接注册表。只在循环线程内访问，因此无需加锁——放在 EventLoop 而不是
    //服务器主类，是因为它必须与该线程的循环同生命周期：连接在循环线程内析构。
    //元素类型取 shared_ptr<void> 是为了不让事件循环依赖连接类型，否则任何
    //用到 EventLoop 的地方都会被拖去链接整个协议层
    void add_connection(const std::shared_ptr<void> &conn);
    //延迟擦除。连接的关闭往往由它自己的回调触发，立刻擦除会让最后一个
    //shared_ptr 在回调栈内析构连接对象
    void remove_connection(const std::shared_ptr<void> &conn);

    TimerQueue *timer_queue() const { return m_timer_queue.get(); }

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

    //在构造时创建：构造发生在目标线程内，timerfd 与它的 Channel 注册都需要
    //该线程的 epoll 实例
    std::unique_ptr<TimerQueue> m_timer_queue;
    std::unordered_set<std::shared_ptr<void>> m_connections;
};

#endif
