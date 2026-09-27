#ifndef CHANNEL_H
#define CHANNEL_H

#include <cstdint>
#include <functional>
#include <memory>
#include <sys/epoll.h>

class EventLoop;

//文件描述符与关注事件的绑定。只标识描述符而不拥有它：
//关闭时机由描述符的所有者决定（连接的归属者、监听的归属者），
//Channel 只保证「关闭之前先把自己从事件表里摘掉」。
//
//触发模式由使用方设置：-m 的监听侧与连接侧组合并不相同，
//而关注事件与 EPOLLET 是同一个 epoll_event 的两部分，因此一并放在这里
class Channel
{
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop *loop, int fd);
    ~Channel();

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    void set_read_callback(EventCallback cb) { m_read_callback = std::move(cb); }
    void set_write_callback(EventCallback cb) { m_write_callback = std::move(cb); }
    void set_close_callback(EventCallback cb) { m_close_callback = std::move(cb); }
    void set_trig_mode(int trig_mode) { m_trig_mode = trig_mode; }

    //绑定持有者的弱引用。handle_event 期间会把它提升为强引用，使「回调执行到
    //一半时持有者被销毁」不会发生：关闭连接的回调正是从这里触发的，而关闭
    //往往意味着持有者的最后一个 shared_ptr 被释放。类型取 void 是为了不依赖
    //持有者的具体类型
    void tie(const std::weak_ptr<void> &owner)
    {
        m_owner = owner;
        m_tied = true;
    }

    void enable_reading();
    void enable_writing();
    void disable_writing();
    void disable_all();
    //从内核事件表摘除。必须在 ::close 之前调用——描述符被内核复用之后，
    //一条迟到的 EPOLL_CTL_DEL 会作用到新连接上
    void remove();

    //按 revents 分派到对应的回调。顺序上读先于关闭：EPOLLRDHUP 只表示对端
    //关闭了写端，接收缓冲区里可能还有最后一个请求，而两者通常同时返回
    void handle_event(uint32_t revents);

    int fd() const { return m_fd; }
    uint32_t events() const { return m_events; }
    //送给 epoll 的完整事件位：关注事件加上触发模式。ET 只影响「何时再通知」，
    //与关注哪些事件无关，因此在这里补齐而不是散落在各次更新里
    uint32_t epoll_events() const { return (1 == m_trig_mode) ? (m_events | EPOLLET) : m_events; }
    bool is_writing() const { return 0 != (m_events & EPOLLOUT); }
    //由 EventLoop 维护：首次注册走 ADD，此后走 MOD
    bool in_epoll() const { return m_in_epoll; }
    void set_in_epoll(bool value) { m_in_epoll = value; }

private:
    void update();

    EventLoop *m_loop; //不持有
    const int m_fd;    //不持有，仅作标识
    uint32_t m_events;
    uint32_t m_revents;
    int m_trig_mode;
    bool m_in_epoll;
    bool m_tied;
    std::weak_ptr<void> m_owner;
    EventCallback m_read_callback;
    EventCallback m_write_callback;
    EventCallback m_close_callback;
};

#endif
