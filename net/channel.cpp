#include "channel.h"
#include "event_loop.h"

namespace
{
//读事件里带上 EPOLLRDHUP：对端关闭写端时也要得到通知，否则只能等到下一次
//读事件才发现连接已经半关闭
const uint32_t kReadEvent = EPOLLIN | EPOLLRDHUP;
const uint32_t kWriteEvent = EPOLLOUT;
} // namespace

Channel::Channel(EventLoop *loop, int fd)
    : m_loop(loop), m_fd(fd), m_events(0), m_revents(0), m_trig_mode(0), m_in_epoll(false), m_tied(false)
{
}

Channel::~Channel()
{
    //这里不摘除事件表项：摘除需要访问 EventLoop，而 Channel 的析构未必早于它。
    //使用方应在销毁之前自行调用 remove()——连接与监听的关闭路径都会这样做
}

//关注事件只在一个方向上：要么等可读、要么等可写，与原先 modfd 的语义一致
void Channel::enable_reading()
{
    //先比较再更新：协议处理在「请求尚未收全」时每次事件都会要求关注可读，
    //每次都发一次 EPOLL_CTL_MOD 会多出与请求数等量的系统调用
    if (kReadEvent == m_events)
        return;

    m_events = kReadEvent;
    update();
}

void Channel::enable_writing()
{
    if (kWriteEvent == m_events)
        return;

    m_events = kWriteEvent;
    update();
}

void Channel::disable_writing()
{
    if (0 != (m_events & kWriteEvent))
        enable_reading();
}

void Channel::disable_all()
{
    if (0 == m_events)
        return;

    m_events = 0;
    update();
}

void Channel::remove()
{
    if (!m_in_epoll)
        return;

    m_loop->remove_channel(this);
}

void Channel::update()
{
    m_loop->update_channel(this);
}

void Channel::handle_event(uint32_t revents)
{
    m_revents = revents;

    //先把持有者提升为强引用：回调里可能触发连接关闭，而关闭意味着持有者的
    //最后一个 shared_ptr 被释放。提升之后，本函数返回之前对象一定存活
    std::shared_ptr<void> guard;
    if (m_tied)
    {
        guard = m_owner.lock();
        if (!guard)
            return; //持有者已销毁，这个描述符的事件不再有意义
    }

    //读先于关闭：EPOLLRDHUP 只表示对端关闭了写端，接收缓冲区里可能还有最后
    //一个请求，而它通常与 EPOLLIN 一起返回。先走关闭会把那个请求丢掉
    if (0 != (revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) && m_read_callback)
        m_read_callback();

    if (0 != (revents & EPOLLOUT) && m_write_callback)
        m_write_callback();

    //放在最后：前面的回调可能已经把连接关掉了，而关闭动作是否真的执行、
    //以及此刻是否还有未完成的工作，判断权在持有者手里（关闭回调应当幂等）
    if (0 != (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) && m_close_callback)
        m_close_callback();
}
