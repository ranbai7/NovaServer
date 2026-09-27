#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <atomic>
#include <memory>
#include <string>

#include "CGImysql/sql_connection_pool.h"
#include "config.h"
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/signal_watcher.h"
#include "net/tcp_connection.h"

//并发连接上限。语义与旧实现的 MAX_FD 不同：旧的是「预分配连接对象的数组容量」，
//现在是「同时在线的连接数上限」——连接对象已按需创建
const long MAX_CONNECTION = 65536;

//空闲连接的回收时间。沿用旧实现里 3 × TIMESLOT（5 秒）的取值，
//使超时行为与改造前一致
const int IDLE_TIMEOUT_MS = 15 * 1000;

//服务器主类。持有主事件循环、监听器与信号接管，负责把新连接交给事件循环
class WebServer
{
public:
    WebServer();
    ~WebServer();

    //读取配置并解析站点根目录
    void init(const Config &config);
    void log_write();
    void sql_pool();
    //建立监听并进入事件循环，直到收到退出信号
    void run();

private:
    void on_new_connection(int connfd, const sockaddr_in &peer);
    void on_connection_closed(const std::shared_ptr<TcpConnection> &conn);

    // ---- 配置 ----
    int m_port;
    int m_close_log;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_thread_num;
    std::string m_root_dir;

    int m_log_write;
    std::string m_log_dir;
    std::string m_log_file;
    int m_log_buf_size;
    int m_log_split_lines;
    int m_log_queue_size;

    std::string m_db_host;
    int m_db_port;
    std::string m_db_user;
    std::string m_db_password;
    std::string m_db_name;
    int m_sql_num;

    // ---- 运行期 ----
    //声明顺序即析构顺序的逆序：事件循环必须最后析构，因为连接与定时器都要
    //在它之前释放
    std::unique_ptr<EventLoop> m_loop;
    std::unique_ptr<SignalWatcher> m_signals;
    std::unique_ptr<Acceptor> m_acceptor;

    char *m_root;
    connection_pool *m_connPool;
    std::atomic<long> m_conn_count;
    int m_listen_trig_mode;
    int m_conn_trig_mode;
};

#endif
