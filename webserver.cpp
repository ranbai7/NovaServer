#include "webserver.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //根目录在 init 中依据配置解析，此处先置空，保证析构函数不会释放未分配的指针
    m_root = nullptr;

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    free(m_root);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(const Config &config)
{
    m_port = config.PORT;
    m_log_write = config.LOGWrite;
    m_OPT_LINGER = config.OPT_LINGER;
    m_TRIGMode = config.TRIGMode;
    m_close_log = config.close_log;
    m_actormodel = config.actor_model;
    m_thread_num = config.thread_num;
    m_sql_num = config.sql_num;

    m_log_dir = config.log_dir;
    m_log_file = config.log_file;
    m_log_buf_size = config.log_buf_size;
    m_log_split_lines = config.log_split_lines;
    m_log_queue_size = config.log_queue_size;

    m_db_host = config.db_host;
    m_db_port = config.db_port;
    m_db_user = config.db_user;
    m_db_password = config.db_password;
    m_db_name = config.db_name;

    //把站点根目录解析为规范化的绝对路径：既去掉 ./ 与重复的 /，
    //也顺带确认该目录确实存在——否则每个请求都会走到 stat 失败为止，问题暴露得太晚。
    //此处尚未初始化日志（日志初始化需要用到配置），因此只能写标准错误
    char resolved[PATH_MAX];
    if (realpath(config.root_dir.c_str(), resolved) == nullptr)
    {
        std::fprintf(stderr, "站点根目录不可用: %s (%s)\n", config.root_dir.c_str(), strerror(errno));
        exit(EXIT_FAILURE);
    }
    //根目录要与请求路径拼进 http_conn 的 m_real_file，装不下时每个请求都无法映射。
    //这类错误只取决于配置，放在启动阶段一次性拦下，比在每个请求里才发现更清楚
    const size_t root_len = strlen(resolved);
    if (root_len > static_cast<size_t>(http_conn::MAX_ROOT_DIR_LEN))
    {
        std::fprintf(stderr, "站点根目录过长: %s（%zu 字符，上限 %d）\n", resolved, root_len,
                     http_conn::MAX_ROOT_DIR_LEN);
        exit(EXIT_FAILURE);
    }
    free(m_root);
    m_root = strdup(resolved);
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 != m_close_log)
        return;

    //日志目录不存在时先建立：Log::init 只负责打开文件，不会创建目录
    if (mkdir(m_log_dir.c_str(), 0755) != 0 && errno != EEXIST)
    {
        std::fprintf(stderr, "创建日志目录 %s 失败: %s\n", m_log_dir.c_str(), strerror(errno));
    }

    //Log 内部的文件名缓冲区放不下更长的名称，超长会被静默截断成另一个文件名
    if (m_log_file.size() >= Log::LOG_NAME_SIZE)
    {
        std::fprintf(stderr, "日志文件名过长: %zu 字符，上限 %zu\n", m_log_file.size(), Log::LOG_NAME_SIZE - 1);
        exit(EXIT_FAILURE);
    }

    //路径形如「目录/文件名」：Log::init 会把最后一个 '/' 之前的部分作为目录、
    //之后的部分作为文件名，并在文件名前追加日期
    const std::string path = m_log_dir + "/" + m_log_file;
    const int queue_size = (1 == m_log_write) ? m_log_queue_size : 0;

    if (!Log::get_instance()->init(path.c_str(), m_close_log, m_log_buf_size, m_log_split_lines, queue_size))
    {
        //日志文件打开失败时改为关闭日志：否则后续写日志会作用于空文件指针
        std::fprintf(stderr, "日志文件 %s 打开失败，已关闭日志\n", path.c_str());
        m_close_log = 1;
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init(m_db_host, m_db_user, m_db_password, m_db_name, m_db_port, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (m_listenfd < 0)
    {
        LOG_ERROR("create socket failed");
        exit(EXIT_FAILURE);
    }

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    if (bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        LOG_ERROR("bind to port %d failed", m_port);
        exit(EXIT_FAILURE);
    }
    if (listen(m_listenfd, 5) < 0)
    {
        LOG_ERROR("listen failed");
        exit(EXIT_FAILURE);
    }

    utils.init(TIMESLOT);

    //epoll创建内核事件表
    m_epollfd = epoll_create(5);
    if (m_epollfd == -1)
    {
        LOG_ERROR("epoll_create failed");
        exit(EXIT_FAILURE);
    }

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    if (socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd) == -1)
    {
        LOG_ERROR("socketpair failed");
        exit(EXIT_FAILURE);
    }
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_db_user, m_db_password,
                       m_db_name);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
