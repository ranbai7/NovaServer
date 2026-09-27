#include "signal_watcher.h"
#include "event_loop.h"

#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace
{
sigset_t make_mask(std::initializer_list<int> signals)
{
    sigset_t mask;
    sigemptyset(&mask);
    for (int signo : signals)
        sigaddset(&mask, signo);
    return mask;
}
} // namespace

bool SignalWatcher::block_signals(std::initializer_list<int> signals)
{
    const sigset_t mask = make_mask(signals);

    //pthread_sigmask 只作用于调用线程，因此这里依赖「调用者尚未创建线程」
    //这一时序约定；若已有子线程存在，它们不会继承新的掩码
    return 0 == pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}

SignalWatcher::SignalWatcher(EventLoop *loop, std::initializer_list<int> signals) : m_signalfd(-1)
{
    const sigset_t mask = make_mask(signals);

    m_signalfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (m_signalfd < 0)
    {
        std::perror("signalfd");
        std::exit(EXIT_FAILURE);
    }

    m_channel.reset(new Channel(loop, m_signalfd));
    m_channel->set_read_callback([this] { handle_read(); });
    m_channel->enable_reading();
}

SignalWatcher::~SignalWatcher()
{
    m_channel->remove();
    close(m_signalfd);
}

void SignalWatcher::handle_read()
{
    //读的长度必须是 signalfd_siginfo 的大小，否则会得到 EINVAL
    signalfd_siginfo info;
    while (read(m_signalfd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info)))
    {
        if (m_callback)
            m_callback(static_cast<int>(info.ssi_signo));
    }
}
