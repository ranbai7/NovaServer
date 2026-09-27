#include "event_loop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/eventfd.h>
#include <unistd.h>

EventLoop::EventLoop() : m_epollfd(-1), m_wakeupfd(-1), m_quitting(false), m_thread_id(std::this_thread::get_id())
{
    //两组描述符都带 CLOEXEC：本服务端不 fork 子进程处理请求，
    //但把「不泄漏给 exec」作为默认更稳妥
    m_epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epollfd < 0)
    {
        std::perror("epoll_create1");
        std::exit(EXIT_FAILURE);
    }

    //唤醒用的是 eventfd 而不是自管道：它是单描述符、固定 8 字节计数的，
    //读写语义比管道简单，也不必为此维护两个 fd
    m_wakeupfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeupfd < 0)
    {
        std::perror("eventfd");
        std::exit(EXIT_FAILURE);
    }

    m_wakeup_channel.reset(new Channel(this, m_wakeupfd));
    m_wakeup_channel->set_read_callback([this] { handle_wakeup(); });
    m_wakeup_channel->enable_reading();

    m_events.resize(kMaxEvents);
}

EventLoop::~EventLoop()
{
    //顺序有依赖：先把唤醒通道从事件表摘除并关闭 eventfd，再关 epoll。
    //待执行任务里可能持有连接的最后一份引用，先于两者清掉
    m_pending_functors.clear();

    if (m_wakeup_channel)
    {
        m_wakeup_channel->remove();
        m_wakeup_channel.reset();
    }

    close(m_wakeupfd);
    close(m_epollfd);
}

void EventLoop::loop()
{
    //不在这里重置 m_quitting：循环启动之前调用 quit() 是合法的，
    //重置会把那次请求丢掉
    while (!m_quitting)
    {
        const int count = epoll_wait(m_epollfd, m_events.data(), static_cast<int>(m_events.size()), -1);
        if (count < 0)
        {
            //被信号打断不是错误，重新等待即可
            if (EINTR == errno)
                continue;

            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < count; ++i)
        {
            Channel *channel = static_cast<Channel *>(m_events[i].data.ptr);
            channel->handle_event(m_events[i].events);
        }

        //本轮的回调里可能又投递了任务
        do_pending_functors();
    }
}

void EventLoop::quit()
{
    m_quitting = true;

    //可能正阻塞在 epoll_wait 上，必须叫醒它才能让循环看到退出标志。
    //本线程调用时亦然：唤醒事件会被本次或下一次循环取走，代价只是一次空转
    if (!is_in_loop_thread())
        wakeup();
}

void EventLoop::run_in_loop(Functor cb)
{
    if (is_in_loop_thread())
        cb();
    else
        queue_in_loop(std::move(cb));
}

void EventLoop::queue_in_loop(Functor cb)
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pending_functors.push_back(std::move(cb));
    }

    //无条件唤醒：任务可能是在某次 epoll_wait 返回之后、进入下一轮之前投递的，
    //不唤醒就要等到下一个事件才有机会执行
    wakeup();
}

void EventLoop::wakeup()
{
    const uint64_t one = 1;
    const ssize_t written = write(m_wakeupfd, &one, sizeof(one));

    //EWOULDBLOCK 表示计数已饱和（事件还没被取走），此时循环必定会被唤醒
    if (written < 0 && EAGAIN != errno && EWOULDBLOCK != errno)
        std::perror("wakeup");
}

void EventLoop::handle_wakeup()
{
    uint64_t value = 0;
    while (read(m_wakeupfd, &value, sizeof(value)) > 0)
    {
        //eventfd 是计数语义：一次读出即清零，循环读是为了把可能的多次计数一并取走
    }
}

void EventLoop::do_pending_functors()
{
    //先在锁内整块换出，再在锁外执行：任务本身可能再次调用 queue_in_loop，
    //持锁执行会自锁
    std::vector<Functor> functors;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        functors.swap(m_pending_functors);
    }

    for (const Functor &fn : functors)
        fn();
}

void EventLoop::update_channel(Channel *channel)
{
    epoll_event event;
    event.data.ptr = channel;
    event.events = channel->epoll_events();

    const int op = channel->in_epoll() ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(m_epollfd, op, channel->fd(), &event) < 0)
    {
        std::perror("epoll_ctl");
        return;
    }

    channel->set_in_epoll(true);
}

void EventLoop::remove_channel(Channel *channel)
{
    if (!channel->in_epoll())
        return;

    //EPOLL_CTL_DEL 的第四个参数在 2.6.9 之后可以传 NULL，此处仍传一个合法指针
    //以兼容更老的实现
    epoll_event event;
    event.data.ptr = channel;
    event.events = 0;

    if (epoll_ctl(m_epollfd, EPOLL_CTL_DEL, channel->fd(), &event) < 0)
        std::perror("epoll_ctl DEL");

    channel->set_in_epoll(false);
}
