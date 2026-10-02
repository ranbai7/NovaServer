#include "acceptor.h"
#include "event_loop.h"
#include "socket_utils.h"

#include "../log/log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
const int kListenBacklog = 512;
} // namespace

Acceptor::Acceptor(EventLoop *loop, int port, int listen_trig_mode, int opt_linger, int close_log)
    : m_loop(loop), m_port(port), m_listen_trig_mode(listen_trig_mode), m_opt_linger(opt_linger),
      m_close_log(close_log), m_listenfd(-1), m_idle_fd(-1)
{
}

Acceptor::~Acceptor()
{
    if (m_channel)
        m_channel->remove();

    close(m_listenfd);
    if (m_idle_fd >= 0)
        close(m_idle_fd);
}

void Acceptor::start()
{
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (m_listenfd < 0)
    {
        LOG_ERROR("create socket failed");
        std::exit(EXIT_FAILURE);
    }

    if (0 == m_opt_linger)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_opt_linger)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    if (bind(m_listenfd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    {
        LOG_ERROR("bind to port %d failed", m_port);
        std::exit(EXIT_FAILURE);
    }

    //backlog 取 512（内核会自行截断到 net.core.somaxconn）。此前的 5 太小：突发建立连接时队列溢出，溢出的连接要等客户端重传 SYN
    if (listen(m_listenfd, kListenBacklog) < 0)
    {
        LOG_ERROR("listen failed");
        std::exit(EXIT_FAILURE);
    }

    //预留一个空闲描述符：描述符耗尽时先释放它，见 handle_fd_exhausted
    m_idle_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

    net::set_nonblocking(m_listenfd);

    m_channel.reset(new Channel(m_loop, m_listenfd));
    m_channel->set_trig_mode(m_listen_trig_mode);
    m_channel->set_read_callback([this] { handle_read(); });
    m_channel->enable_reading();
}

void Acceptor::handle_read()
{
    while (true)
    {
        sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int connfd = accept(m_listenfd, reinterpret_cast<sockaddr *>(&peer), &peer_len);

        if (connfd >= 0)
        {
            net::set_nonblocking(connfd);
            accept_one(connfd, peer);

            //LT 下一次只取一个：单个监听描述符不该长期占用事件循环
            if (0 == m_listen_trig_mode)
                break;
            continue;
        }

        if (EAGAIN == errno || EWOULDBLOCK == errno)
            break; //待处理的连接已取完，是 ET 循环的正常出口

        //被信号打断或连接在 accept 前已被对端中止：队列里可能还有连接而 ET 不再通知，必须重试
        if (EINTR == errno || ECONNABORTED == errno)
            continue;

        if (EMFILE == errno || ENFILE == errno)
        {
            handle_fd_exhausted();
            continue;
        }

        LOG_ERROR("accept failed, errno is %d", errno);
        break;
    }
}

void Acceptor::accept_one(int connfd, const sockaddr_in &peer)
{
    if (m_callback)
        m_callback(connfd, peer);
}

void Acceptor::handle_fd_exhausted()
{
    //描述符耗尽时 accept 以 EMFILE 失败，而 ET 下它不会再触发下一次事件，服务将永久失去接受能力：
    //先关掉预留的空闲描述符，accept 一个连接（此时必然成功）后立刻关闭，再把空闲描述符占回来
    if (m_idle_fd >= 0)
    {
        close(m_idle_fd);
        m_idle_fd = -1;
    }

    sockaddr_in dummy;
    socklen_t dummy_len = sizeof(dummy);
    const int fd = accept(m_listenfd, reinterpret_cast<sockaddr *>(&dummy), &dummy_len);
    if (fd >= 0)
        close(fd);

    m_idle_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (m_idle_fd < 0)
        LOG_ERROR("failed to reserve an idle fd, errno is %d", errno);
}
