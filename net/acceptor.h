#ifndef ACCEPTOR_H
#define ACCEPTOR_H

#include <functional>
#include <memory>
#include <netinet/in.h>

#include "channel.h"

class EventLoop;

//监听套接字的管理：建立、接受新连接、描述符耗尽兜底；新连接回调运行在主 Reactor 线程内
class Acceptor
{
public:
    using NewConnectionCallback = std::function<void(int connfd, const sockaddr_in &peer)>;

    Acceptor(EventLoop *loop, int port, int listen_trig_mode, int opt_linger, int close_log);
    ~Acceptor();

    Acceptor(const Acceptor &) = delete;
    Acceptor &operator=(const Acceptor &) = delete;

    void set_new_connection_callback(NewConnectionCallback cb) { m_callback = std::move(cb); }
    //建立监听套接字并开始接受连接。失败即终止：没有监听端口的服务端没有意义
    void start();

    int listen_fd() const { return m_listenfd; }

private:
    void handle_read();
    void accept_one(int connfd, const sockaddr_in &peer);
    //描述符耗尽时的一次「建立即关闭」，见 handle_read 中的说明
    void handle_fd_exhausted();

    EventLoop *m_loop;
    const int m_port;
    const int m_listen_trig_mode;
    const int m_opt_linger;
    const int m_close_log;
    int m_listenfd;
    int m_idle_fd; //预留的空闲描述符，用于描述符耗尽时恢复接受能力
    std::unique_ptr<Channel> m_channel;
    NewConnectionCallback m_callback;
};

#endif
