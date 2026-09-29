#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <netinet/in.h>

#include "../http/http_conn.h"
#include "channel.h"

class EventLoop;

//一条连接：持有描述符、通道与协议对象，负责这三者的生命周期。
//
//整个生命周期——构造、读写、析构——都发生在所属事件循环的线程内，因此协议状态
//不需要任何跨线程保护，「同一连接上读写串行」是结构推论。对象一经 make_shared
//创建地址即固定，协议对象内部那些指向自身缓冲的裸指针因此始终有效
class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
public:
    //关闭后的收尾回调（从注册表移除等），由服务器主类提供
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection> &)>;

    TcpConnection(EventLoop *loop, int connfd, const sockaddr_in &peer, const char *root, int trig_mode, int close_log,
                  connection_pool *connPool, int idle_timeout_ms);
    ~TcpConnection();

    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;

    //注册描述符并装载空闲定时器。必须在 make_shared 之后调用：定时器回调持有
    //本对象的弱引用，要求它已经被 shared_ptr 持有
    void start();
    //释放的唯一出口。对端关闭、读写失败、空闲超时、服务器退出都汇聚到这里
    void close();
    void set_close_callback(CloseCallback cb) { m_close_callback = std::move(cb); }

    int fd() const { return m_fd; }
    const sockaddr_in &peer() const { return m_peer; }
    EventLoop *loop() const { return m_loop; }

private:
    void handle_read();
    void handle_write();
    void handle_close();
    void refresh_idle_timer();
    //实际释放描述符与映射。与 close() 分开是因为它要能在析构里调用——
    //析构里不能调 shared_from_this()
    void release_fd();

    EventLoop *m_loop;
    int m_fd; //本对象是它的唯一所有者
    sockaddr_in m_peer;
    int m_idle_timeout_ms;
    std::unique_ptr<Channel> m_channel;
    http_conn m_conn;    //协议对象；本对象地址恒定，其内部裸指针因此始终有效
    uint64_t m_timer_id; //0 表示没有登记
    bool m_closed;
    CloseCallback m_close_callback;
};

#endif
