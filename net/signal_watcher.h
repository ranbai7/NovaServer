#ifndef SIGNAL_WATCHER_H
#define SIGNAL_WATCHER_H

#include <csignal>
#include <functional>
#include <initializer_list>
#include <memory>

#include "channel.h"

class EventLoop;

//用 signalfd 接管进程信号：先在所有线程内屏蔽这些信号，再让它们只经 signalfd 送达——信号处理便变成
//普通的事件驱动读取，不必写异步信号安全的处理函数。旧实现那个函数只能「往自管道写一个字节」，正是这种限制的体现
class SignalWatcher
{
public:
    using SignalCallback = std::function<void(int signo)>;

    SignalWatcher(EventLoop *loop, std::initializer_list<int> signals);
    ~SignalWatcher();

    SignalWatcher(const SignalWatcher &) = delete;
    SignalWatcher &operator=(const SignalWatcher &) = delete;

    void set_callback(SignalCallback cb) { m_callback = std::move(cb); }

    //在所有线程内屏蔽这些信号，使它们不再触发处理函数、只留在待处理集合里由 signalfd 读出。
    //**必须在创建任何线程之前调用**：掩码是线程属性、子线程继承创建时的掩码，晚调用的部分线程不会被屏蔽，信号可能被它们抢先处理
    static bool block_signals(std::initializer_list<int> signals);

private:
    void handle_read();

    int m_signalfd;
    std::unique_ptr<Channel> m_channel;
    SignalCallback m_callback;
};

#endif
