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

//单线程事件循环：一个 epoll 实例、一个跨线程唤醒的 eventfd，以及一个待执行任务队列。归属约定是结构性的
//——事件处理、连接状态与超时定时器都只在本线程内访问，跨线程只能经 run_in_loop/queue_in_loop 投递；
//约定成立后「同一连接上读写串行」不需附加机制
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

    //连接注册表。只在循环线程内访问，无需加锁；放在 EventLoop 而非主类，因它必须与该线程循环同生命周期。
    //元素取 shared_ptr<void> 以免事件循环依赖连接类型（否则用到 EventLoop 处都要链接整个协议层）
    void add_connection(const std::shared_ptr<void> &conn);
    //延迟擦除：连接的关闭往往由它自己的回调触发，立刻擦除会让最后一个 shared_ptr 在回调栈内析构对象
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

    //在构造时创建：构造发生在目标线程内，timerfd 与它的 Channel 注册都需要该线程的 epoll 实例
    std::unique_ptr<TimerQueue> m_timer_queue;
    std::unordered_set<std::shared_ptr<void>> m_connections;
};

#endif
