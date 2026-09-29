#ifndef LOG_H
#define LOG_H

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

//日志初始化参数。
//
//用结构体而不是一串位置参数：这些取值里有五个是 int，相邻两个写反编译器不会
//报错，运行期只表现为「某个配置没生效」，读代码时也不易发现。改用具名字段后
//这类错误在阅读时就能被看出
struct LogConfig
{
    std::string dir;  //日志目录，可为空（相对路径以工作目录为基准）
    std::string file; //文件名前缀，实际文件名在其前追加日期

    int close_log = 0;  //关闭日志：0 = 打开，1 = 关闭
    int write_mode = 1; //写入方式：0 = 同步（调用方自己排空），1 = 异步（写盘线程批量落盘）

    int log_buf_size = 2000;      //单条日志的长度上限，会被夹到 [MIN_LINE_BUF, MAX_LINE_BUF]
    int split_lines = 800000;     //单个文件的**软**行数上限，见 log.cpp 的轮转说明
    int batch_buf_size = 65536;   //缓冲块大小：落盘以整块为单位
    int flush_interval_ms = 1000; //异步模式的定时刷新间隔
};

class Log
{
public:
    //日志文件名部分的缓冲区容量，含结尾的 '\0'。调用方（WebServer）据此
    //在启动时校验配置中的文件名长度，避免超长名称被静默截断
    static const size_t LOG_NAME_SIZE = 128;

    //单条日志的硬上限。格式化缓冲是线程私有的定长数组，容量必须在编译期确定：
    //若改用按配置扩容的缓冲，热路径就得无锁读一个会被 init 改写的量（那是数据
    //竞争），或者为它加锁。把取值范围固定下来，这个问题就不存在了——init 只需
    //把配置夹进这个区间
    static const int MIN_LINE_BUF = 128;
    static const int MAX_LINE_BUF = 4096;

    //其余配置项的合法区间。WebServer 在启动阶段据此校验配置（与触发模式、线程数
    //一样，取值非法就直接终止），Log::init 内部再夹一次作为兜底
    static const int MIN_BATCH_BUF_SIZE = MAX_LINE_BUF; //一块至少要装得下一行
    static const int MAX_BATCH_BUF_SIZE = 8 * 1024 * 1024;
    static const int MIN_FLUSH_INTERVAL_MS = 1;
    static const int MAX_FLUSH_INTERVAL_MS = 3600 * 1000;
    static const size_t MAX_PENDING_BLOCKS = 8; //待写队列长度上限，见 log.cpp

    //C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    //可重复调用：会先停掉上一个写盘线程、排空并关闭上一个文件
    bool init(const LogConfig &config);

    void write_log(int level, const char *format, ...);

    //排空并等待落盘完成。供进程退出与单元测试使用
    void flush(void);

private:
    Log();
    ~Log();
    Log(const Log &) = delete;
    Log &operator=(const Log &) = delete;

    //一块定长缓冲，是落盘的最小单位。行不跨块（见 write_log），
    //因此写盘侧只需整块写出，不必按行切分
    struct Batch
    {
        std::unique_ptr<char[]> data;
        size_t cap = 0;       //容量，等于 batch_buf_size
        size_t used = 0;      //已写入字节数，恒落在整行边界上
        size_t lines = 0;     //本块的行数，轮转计数与丢弃核对用它
        time_t first_sec = 0; //**首行**的时间戳：日切按它判定，而不是按落盘时刻
    };

    void writer_loop();
    void shutdown();

    //同步模式：把一行直接写文件。不做缓冲，写入顺序即文件顺序
    void write_line_direct(const char *data, size_t len, time_t sec);
    //把 front 交给待写队列。返回 false 表示池中没有空闲块（背压）
    bool retire_front_locked();
    //排空待写队列。调用时必须持有 m_mutex；写文件的部分在锁外完成
    void drain_locked(std::unique_lock<std::mutex> &lock);
    void write_batch(const Batch &batch);
    void rotate_if_needed_locked(const Batch &batch);
    void reopen_locked(const std::string &file_name);
    void write_dropped_locked(long long dropped);

    // ---- 缓冲池（仅异步模式使用）----
    std::mutex m_mutex;
    std::condition_variable m_cv;       //换块 → 唤醒写盘线程
    std::condition_variable m_flush_cv; //排空完成 → 唤醒 flush 的等待者
    std::vector<std::unique_ptr<Batch>> m_pool;
    Batch *m_front = nullptr;       //当前写入块
    std::vector<Batch *> m_pending; //已满、待落盘，FIFO。有且只有写盘线程会取走它，
                                    //这是「文件内容的顺序等于写入顺序」的保证所在
    std::vector<Batch *> m_free;    //空闲块
    uint64_t m_seq = 0;             //每有一块进入待写队列自增，即该块的序号
    uint64_t m_drained = 0;         //已落盘的最后一块的序号
    std::atomic<long long> m_dropped{0};

    // ---- 文件侧 ----
    //单独一把锁，保护的是「文件位置」这份状态，与缓冲池状态无关。它的意义是把
    //「同一时刻只有一个写文件的人」变成结构约束而不是约定：同步模式下排空的是
    //调用线程，异步模式下是写盘线程，两者都经 write_batch 进这把锁。
    //锁序：m_mutex 与 m_file_mutex 从不同时持有，因此不存在锁序问题
    std::mutex m_file_mutex;
    FILE *m_fp = nullptr;
    long long m_line_count = 0; //当前文件的已写行数
    int m_split_index = 0;      //当前文件的序号后缀，0 表示无后缀
    int m_day_key = 0;          //当前文件的 年 * 10000 + 月 * 100 + 日
    std::string m_dir_name;     //目录部分（含末尾 '/'）或空
    std::string m_log_name;     //文件名部分
    int m_split_lines = 800000; //软上限，见 rotate_if_needed_locked

    // ---- 运行状态 ----
    std::atomic<bool> m_ready{false}; //init 成功后置位；write_log 与 flush 先读它
    std::thread m_writer;
    bool m_writer_active = false; //仅 m_mutex 下访问；同时表达「是否需要调用方自己排空」
    bool m_stopping = false;      //同上
    int m_flush_interval_ms = 1000;
    //夹取后的单行上限。热路径无锁读它，因此用 atomic
    std::atomic<int> m_line_cap{2000};
};

//写入日志后不再逐行落盘：写盘线程按 flush_interval 或缓冲块写满时批量落盘。
//调用路径上没有系统调用，换块时才会唤醒写盘线程。
//
//宏沿用调用作用域里的 m_close_log 成员（http_conn、Acceptor、connection_pool
//各自持有同名成员）；用 do-while 包住，避免 `if (x) LOG_INFO(...); else ...`
//这类写法把 else 错挂到宏内的 if 上
#define LOG_DEBUG(format, ...)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (0 == m_close_log)                                                                                          \
        {                                                                                                              \
            Log::get_instance()->write_log(0, format, ##__VA_ARGS__);                                                  \
        }                                                                                                              \
    } while (0)

#define LOG_INFO(format, ...)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (0 == m_close_log)                                                                                          \
        {                                                                                                              \
            Log::get_instance()->write_log(1, format, ##__VA_ARGS__);                                                  \
        }                                                                                                              \
    } while (0)

#define LOG_WARN(format, ...)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (0 == m_close_log)                                                                                          \
        {                                                                                                              \
            Log::get_instance()->write_log(2, format, ##__VA_ARGS__);                                                  \
        }                                                                                                              \
    } while (0)

//错误行保留一次落盘：错误是排障时最不能丢的信息，而按间隔批量落盘意味着进程
//崩溃时最多丢一个间隔的日志。错误路径罕见，等写盘线程的实际开销在微秒级
#define LOG_ERROR(format, ...)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (0 == m_close_log)                                                                                          \
        {                                                                                                              \
            Log::get_instance()->write_log(3, format, ##__VA_ARGS__);                                                  \
            Log::get_instance()->flush();                                                                              \
        }                                                                                                              \
    } while (0)

#endif
