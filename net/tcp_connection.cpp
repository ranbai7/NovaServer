#include "tcp_connection.h"
#include "event_loop.h"
#include "timer_queue.h"

#include <cstdio>
#include <unistd.h>

TcpConnection::TcpConnection(EventLoop *loop, int connfd, const sockaddr_in &peer, const char *root, int trig_mode,
                             int close_log, connection_pool *connPool, int idle_timeout_ms)
    : m_loop(loop), m_fd(connfd), m_peer(peer), m_idle_timeout_ms(idle_timeout_ms), m_timer_id(0), m_closed(false)
{
    m_channel.reset(new Channel(loop, connfd));
    m_channel->set_trig_mode(trig_mode);
    m_conn.init(connfd, peer, root, trig_mode, close_log, connPool);
}

TcpConnection::~TcpConnection()
{
    //兜底：正常路径已由 close() 释放过，这里因幂等而是空操作
    release_fd();
}

void TcpConnection::start()
{
    //协议层经这两个回调表达「接下来关心什么事件」，实际注册由本对象完成；不注入的话协议层退化为空操作（构造函数缺省值），表现为响应组装好了却不转去关注可写事件
    m_conn.set_event_notifier([this] { m_channel->enable_reading(); }, [this] { m_channel->enable_writing(); });

    m_channel->set_read_callback([this] { handle_read(); });
    m_channel->set_write_callback([this] { handle_write(); });
    m_channel->set_close_callback([this] { handle_close(); });
    //绑上弱引用：回调里关闭连接会释放最后一份强引用，无此保护则 handle_event 返回后访问的是已析构对象
    m_channel->tie(weak_from_this());
    m_channel->enable_reading();

    //空闲定时器持有弱引用：连接先于定时器销毁时回调取到空指针直接丢弃；此前按 fd 下标访问连接槽位，fd 被内核复用后会命中另一条连接
    const std::weak_ptr<TcpConnection> weak = weak_from_this();
    m_timer_id = m_loop->timer_queue()->add_timer(m_idle_timeout_ms,
                                                  [weak]
                                                  {
                                                      if (const std::shared_ptr<TcpConnection> conn = weak.lock())
                                                          conn->close();
                                                  });
}

void TcpConnection::close()
{
    //五个触发点（读失败、处理失败、写失败、对端关闭、空闲超时）都汇聚到这里，因此这里必须幂等
    if (m_closed)
        return;

    m_closed = true;
    release_fd();

    if (m_close_callback)
        m_close_callback(shared_from_this());
}

void TcpConnection::release_fd()
{
    if (0 != m_timer_id)
    {
        m_loop->timer_queue()->cancel_timer(m_timer_id);
        m_timer_id = 0;
    }

    //映射可能建立在一个尚未写出响应的请求上，先于描述符释放
    m_conn.unmap();

    if (m_fd >= 0)
    {
        //顺序不可颠倒：描述符被内核复用之后，一条迟到的 EPOLL_CTL_DEL 会作用到新连接上
        m_channel->disable_all();
        m_channel->remove();
        ::close(m_fd);
        m_fd = -1;
    }
}

void TcpConnection::handle_read()
{
    if (!m_conn.read_once())
    {
        //对端关闭或读取出错
        close();
        return;
    }

    refresh_idle_timer();

    //process 返回 false 表示该连接应当关闭（响应没能构造出来等）
    if (!m_conn.process())
        close();
}

void TcpConnection::handle_write()
{
    if (!m_conn.write())
    {
        close();
        return;
    }

    refresh_idle_timer();

    //管线化：一次读入可能含多个请求，前一个响应发完后读缓冲里可能还剩着下一个（reset 保留已完整解析时剩下的字节）；立刻接着处理，否则要等对端再发数据才动
    if (m_conn.has_pending_work() && !m_conn.process())
        close();
}

void TcpConnection::handle_close()
{
    //对端关闭写端或连接出错。只有确实无事可做时才关闭：请求可能刚读完、响应也可能还没发完，而半关闭事件常与读写事件一起返回
    if (m_conn.has_pending_work())
        return; //由读或写路径收尾

    close();
}

void TcpConnection::refresh_idle_timer()
{
    if (0 != m_timer_id)
        m_loop->timer_queue()->refresh_timer(m_timer_id, m_idle_timeout_ms);
}
