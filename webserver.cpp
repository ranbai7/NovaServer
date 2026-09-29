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
    //线程池必须最先停。它会 join 所有子线程，而子线程可能仍在处理连接——
    //那些连接的协议对象持有 m_root（作为 doc_root），要等它们全部退出之后
    //才能释放。析构函数体先于成员析构执行，若不在这里显式停掉，
    //free(m_root) 就会跑在 m_thread_pool 析构之前，与还在读 m_root 的子线程
    //构成数据竞争
    m_thread_pool.reset();

    //随后释放监听与信号，最后释放事件循环：后两者的析构都要访问循环
    m_acceptor.reset();
    m_signals.reset();
    m_loop.reset();

    free(m_root);

    //最后排空日志。异步写入下日志按缓冲块批量落盘，此处之前写下的内容还在内存里；
    //放在所有组件析构之后，是为了把销毁过程本身产生的日志也一并落盘。
    //
    //不依赖 Log 单例的析构函数：它是函数内的 static，其析构排在退出阶段的静态析构
    //序列里，顺序不确定，可能晚于其它同样会写日志的静态对象
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

    //触发模式的两侧组合。取值非法时终止启动：此前这里没有 else 分支，
    //而两个成员又未初始化，写错配置会以不确定的方式注册描述符
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

    //子 Reactor 线程数不得为负。0 是合法取值——不建子线程，全部连接归主循环，
    //用于与多线程分发做对照；负数则只会被线程池静默当作 0，让「改了参数却没生效」
    //看起来像是没有收益，因此在启动阶段直接拦下
    if (m_thread_num < 0)
    {
        std::fprintf(stderr, "子 Reactor 线程数非法: %d（须不小于 0）\n", m_thread_num);
        exit(EXIT_FAILURE);
    }

    //把站点根目录解析为规范化的绝对路径：既去掉 ./ 与重复的 /，
    //也顺带确认该目录确实存在——否则每个请求都会走到 stat 失败为止，问题暴露得太晚。
    //此处尚未初始化日志（日志初始化需要用到配置），因此只能写标准错误
    char resolved[PATH_MAX];
    if (realpath(m_root_dir.c_str(), resolved) == nullptr)
    {
        std::fprintf(stderr, "站点根目录不可用: %s (%s)\n", m_root_dir.c_str(), strerror(errno));
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

    //日志的这几项取值直接参与缓冲区下标计算，越界取值会让日志子系统在「看起来
    //一切正常」的情况下出错（例如单行上限小于时间前缀的长度时会写越界）。
    //与触发模式、线程数一样，在启动阶段一次性拦下，而不是留给 Log 内部去夹取
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
    //信号必须在创建任何线程之前屏蔽。当前只有主线程，但这道约束要写在
    //它将来仍然成立的位置上——线程池一旦建立，晚屏蔽就无效了
    if (!SignalWatcher::block_signals({SIGTERM, SIGINT}))
        std::fprintf(stderr, "屏蔽退出信号失败: %s\n", strerror(errno));

    //SIGPIPE 仍按忽略处理：客户端提前断开时写操作会收到它，
    //而写失败已经在返回值里体现了，不需要让进程收到信号
    signal(SIGPIPE, SIG_IGN);

    m_signals.reset(new SignalWatcher(m_loop.get(), {SIGTERM, SIGINT}));
    m_signals->set_callback([this](int) { m_loop->quit(); });

    //线程池在建线程之前启动信号接管：信号掩码是线程属性，子线程会继承
    //创建时的掩码，晚屏蔽就无效了
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

    //连接归某个子循环所有：把建立过程投递到那个线程，此后它的读写、协议解析
    //与超时定时器都在那里完成，跨线程只传递这一次连接对象
    EventLoop *loop = m_thread_pool->next_loop();
    loop->run_in_loop(
        [this, loop, connfd, peer]
        {
            const std::shared_ptr<TcpConnection> conn = std::make_shared<TcpConnection>(
                loop, connfd, peer, m_root, m_conn_trig_mode, m_close_log, IDLE_TIMEOUT_MS);
            conn->set_close_callback([this](const std::shared_ptr<TcpConnection> &closed)
                                     { on_connection_closed(closed); });

            //顺序固定：先登记再启动。start() 里注册的定时器持有连接的弱引用，
            //要求它此前已经被 shared_ptr 持有
            loop->add_connection(conn);
            conn->start();
        });

    LOG_INFO("new connection, active: %ld", m_conn_count.load());
}

void WebServer::on_connection_closed(const std::shared_ptr<TcpConnection> &conn)
{
    //延迟擦除：本函数由连接自己的回调触发，立刻从注册表移除会让最后一份
    //引用在回调栈内析构连接对象
    conn->loop()->remove_connection(conn);

    LOG_INFO("connection closed, active: %ld", m_conn_count.fetch_sub(1) - 1);
}
