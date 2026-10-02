#include "webserver.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

WebServer::WebServer()
{
    //根目录在 init 中依据配置解析，此处先置空，保证析构函数不会释放未分配的指针
    m_root = nullptr;
    m_connPool = nullptr;

    m_loop.reset(new EventLoop());
    m_acceptor.reset();
    m_signals.reset();

    m_conn_count = 0;
    m_listen_trig_mode = 0;
    m_conn_trig_mode = 0;
}

WebServer::~WebServer()
{
    //线程池必须最先停：它会 join 所有子线程，而子线程的协议对象仍持有 m_root（作为
    //doc_root），须等它们全部退出才能释放。析构函数体先于成员析构执行，若不在此显式
    //停掉，free(m_root) 会先于 m_thread_pool 析构运行，与仍在读 m_root 的子线程竞争
    m_thread_pool.reset();

    //随后释放监听与信号，最后释放事件循环：后两者的析构都要访问循环
    m_acceptor.reset();
    m_signals.reset();
    m_loop.reset();

    free(m_root);

    //最后排空日志：异步写入下日志按缓冲块批量落盘，此前写下的内容还在内存里；放在所有
    //组件析构之后，是为把销毁过程本身产生的日志也一并落盘。不依赖 Log 单例的析构函数：
    //它是函数内 static，析构顺序不确定，可能晚于其它同样会写日志的静态对象
    Log::get_instance()->flush();
}

void WebServer::init(const Config &config)
{
    m_port = config.PORT;
    m_close_log = config.close_log;
    m_log_write = config.LOGWrite;
    m_OPT_LINGER = config.OPT_LINGER;
    m_TRIGMode = config.TRIGMode;
    m_thread_num = config.thread_num;
    m_sql_num = config.sql_num;
    m_root_dir = config.root_dir;

    m_log_dir = config.log_dir;
    m_log_file = config.log_file;
    m_log_buf_size = config.log_buf_size;
    m_log_split_lines = config.log_split_lines;
    m_log_batch_buf_size = config.log_batch_buf_size;
    m_log_flush_interval = config.log_flush_interval;

    m_db_host = config.db_host;
    m_db_port = config.db_port;
    m_db_user = config.db_user;
    m_db_password = config.db_password;
    m_db_name = config.db_name;

    //触发模式的两侧组合。取值非法即终止启动：此前没有 else 分支且两成员未初始化，写错会以不确定方式注册描述符
    switch (m_TRIGMode)
    {
    case 0: //LT + LT
        m_listen_trig_mode = 0;
        m_conn_trig_mode = 0;
        break;
    case 1: //LT + ET
        m_listen_trig_mode = 0;
        m_conn_trig_mode = 1;
        break;
    case 2: //ET + LT
        m_listen_trig_mode = 1;
        m_conn_trig_mode = 0;
        break;
    case 3: //ET + ET
        m_listen_trig_mode = 1;
        m_conn_trig_mode = 1;
        break;
    default:
        std::fprintf(stderr, "触发模式取值非法: %d（有效范围 0..3）\n", m_TRIGMode);
        exit(EXIT_FAILURE);
    }

    //线程数不得为负：0 合法（全部连接归主循环，用于对照），负数会被线程池静默当作 0，让「改了参数却没生效」不易察觉，故启动阶段拦下
    if (m_thread_num < 0)
    {
        std::fprintf(stderr, "子 Reactor 线程数非法: %d（须不小于 0）\n", m_thread_num);
        exit(EXIT_FAILURE);
    }

    //以下几项直接进入系统调用参数：端口超出 16 位会被 htons 截断绑到另一端，连接数为 0 则连接池为空、启动期查用户表拿到空句柄
    if (m_port < 1 || m_port > 65535)
    {
        std::fprintf(stderr, "端口取值非法: %d（有效范围 1..65535）\n", m_port);
        exit(EXIT_FAILURE);
    }
    if (m_db_port < 1 || m_db_port > 65535)
    {
        std::fprintf(stderr, "数据库端口取值非法: %d（有效范围 1..65535）\n", m_db_port);
        exit(EXIT_FAILURE);
    }
    if (m_sql_num < 1)
    {
        std::fprintf(stderr, "数据库连接数非法: %d（须不小于 1）\n", m_sql_num);
        exit(EXIT_FAILURE);
    }
    if (m_OPT_LINGER != 0 && m_OPT_LINGER != 1)
    {
        std::fprintf(stderr, "关闭连接方式非法: %d（0 = 不使用，1 = 使用）\n", m_OPT_LINGER);
        exit(EXIT_FAILURE);
    }
    if (m_close_log != 0 && m_close_log != 1)
    {
        std::fprintf(stderr, "关闭日志开关非法: %d（0 = 打开，1 = 关闭）\n", m_close_log);
        exit(EXIT_FAILURE);
    }

    //把站点根目录解析为规范化的绝对路径，兼确认目录存在——否则每个请求都走到 stat 失败太晚；此时日志尚未初始化，只能写标准错误
    char resolved[PATH_MAX];
    if (realpath(m_root_dir.c_str(), resolved) == nullptr)
    {
        std::fprintf(stderr, "站点根目录不可用: %s (%s)\n", m_root_dir.c_str(), strerror(errno));
        exit(EXIT_FAILURE);
    }
    //根目录要与请求路径拼进 http_conn 的 m_real_file，装不下则每个请求都无法映射；只取决于配置，启动阶段一次性拦下更清楚
    const size_t root_len = strlen(resolved);
    if (root_len > static_cast<size_t>(http_conn::MAX_ROOT_DIR_LEN))
    {
        std::fprintf(stderr, "站点根目录过长: %s（%zu 字符，上限 %d）\n", resolved, root_len,
                     http_conn::MAX_ROOT_DIR_LEN);
        exit(EXIT_FAILURE);
    }
    free(m_root);
    m_root = strdup(resolved);

    m_acceptor.reset(new Acceptor(m_loop.get(), m_port, m_listen_trig_mode, m_OPT_LINGER, m_close_log));
    m_acceptor->set_new_connection_callback([this](int connfd, const sockaddr_in &peer)
                                            { on_new_connection(connfd, peer); });

    //子 Reactor 线程池，连接按轮转分配给它们
    m_thread_pool.reset(new EventLoopThreadPool(m_loop.get(), m_thread_num));
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

    //写入方式：0 = 同步（每行直接落盘），1 = 异步（缓冲池批量落盘）
    if (m_log_write != 0 && m_log_write != 1)
    {
        std::fprintf(stderr, "日志写入方式非法: %d（0 = 同步，1 = 异步）\n", m_log_write);
        exit(EXIT_FAILURE);
    }

    //这几项直接参与缓冲区下标计算，越界会让日志子系统在「看起来正常」时出错；与触发模式、线程数一样在启动阶段一次性拦下
    const auto check_range = [](const char *name, int value, int lo, int hi)
    {
        if (value < lo || value > hi)
        {
            std::fprintf(stderr, "%s 取值非法: %d（有效范围 %d..%d）\n", name, value, lo, hi);
            exit(EXIT_FAILURE);
        }
    };
    check_range("日志单行上限 buf_size", m_log_buf_size, Log::MIN_LINE_BUF, Log::MAX_LINE_BUF);
    check_range("日志缓冲块大小 batch_buf_size", m_log_batch_buf_size, Log::MIN_BATCH_BUF_SIZE,
                Log::MAX_BATCH_BUF_SIZE);
    check_range("日志刷新间隔 flush_interval", m_log_flush_interval, Log::MIN_FLUSH_INTERVAL_MS,
                Log::MAX_FLUSH_INTERVAL_MS);
    if (m_log_split_lines < 1)
    {
        std::fprintf(stderr, "日志文件行数上限非法: %d（须不小于 1）\n", m_log_split_lines);
        exit(EXIT_FAILURE);
    }

    LogConfig config;
    config.dir = m_log_dir;
    config.file = m_log_file;
    config.close_log = m_close_log;
    config.write_mode = m_log_write;
    config.log_buf_size = m_log_buf_size;
    config.split_lines = m_log_split_lines;
    config.batch_buf_size = m_log_batch_buf_size;
    config.flush_interval_ms = m_log_flush_interval;

    const std::string path = m_log_dir + "/" + m_log_file;
    if (!Log::get_instance()->init(config))
    {
        //日志文件打开失败时改为关闭日志：否则后续写日志会作用于空文件指针
        std::fprintf(stderr, "日志文件 %s 打开失败，已关闭日志\n", path.c_str());
        m_close_log = 1;
    }
}

void WebServer::sql_pool()
{
    m_connPool = connection_pool::GetInstance();
    m_connPool->init(m_db_host, m_db_user, m_db_password, m_db_name, m_db_port, m_sql_num, m_close_log);

    //启动期一次性加载用户表
    http_conn::initmysql_result(m_connPool, m_close_log);
}

void WebServer::run()
{
    //退出信号的屏蔽已在 main() 开头完成——它必须早于任何线程的创建，而日志写盘线程在此前就建好，故那步不能留在这里；此处只把信号接到事件循环

    //SIGPIPE 按忽略处理：客户端提前断开时写操作会收到它，而写失败已在返回值里体现，无需让进程收到信号
    signal(SIGPIPE, SIG_IGN);

    m_signals.reset(new SignalWatcher(m_loop.get(), {SIGTERM, SIGINT}));
    m_signals->set_callback([this](int) { m_loop->quit(); });

    m_thread_pool->start();
    m_acceptor->start();
    m_loop->loop();
}

void WebServer::on_new_connection(int connfd, const sockaddr_in &peer)
{
    if (m_conn_count.fetch_add(1) >= MAX_CONNECTION)
    {
        m_conn_count.fetch_sub(1);

        //超出上限：给出一条提示再关闭。此时该描述符尚未注册进事件循环
        const char *busy = "Internal server busy";
        if (send(connfd, busy, strlen(busy), 0) < 0)
        {
            //对端可能已经断开，写失败不是这里的问题
        }
        close(connfd);

        LOG_ERROR("%s", "Internal server busy");
        return;
    }

    //连接归某个子循环所有：建立过程投递到那个线程，此后的读写、解析与超时定时器都在那里完成，只跨线程传递一次连接对象
    EventLoop *loop = m_thread_pool->next_loop();
    loop->run_in_loop(
        [this, loop, connfd, peer]
        {
            const std::shared_ptr<TcpConnection> conn = std::make_shared<TcpConnection>(
                loop, connfd, peer, m_root, m_conn_trig_mode, m_close_log, m_connPool, IDLE_TIMEOUT_MS);
            conn->set_close_callback([this](const std::shared_ptr<TcpConnection> &closed)
                                     { on_connection_closed(closed); });

            //顺序固定：先登记再启动——start() 注册的定时器持有连接弱引用，要求它此前已被 shared_ptr 持有
            loop->add_connection(conn);
            conn->start();
        });

    LOG_INFO("new connection, active: %ld", m_conn_count.load());
}

void WebServer::on_connection_closed(const std::shared_ptr<TcpConnection> &conn)
{
    //延迟擦除：本函数由连接自己的回调触发，立刻从注册表移除会让最后一份引用在回调栈内析构连接对象
    conn->loop()->remove_connection(conn);

    LOG_INFO("connection closed, active: %ld", m_conn_count.fetch_sub(1) - 1);
}
