#include "log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace
{
//线程私有格式化缓冲。用定长数组而非按需扩容：容量须编译期确定，否则热路径要无锁读会被 init 改写的量（数据竞争）
thread_local char t_line[Log::MAX_LINE_BUF];

const char *level_tag(int level)
{
    switch (level)
    {
    case 0:
        return "[debug]:";
    case 2:
        return "[warn]:";
    case 3:
        return "[erro]:";
    default:
        return "[info]:";
    }
}

//把配置夹进 [lo, hi]：配置直接参与缓冲区下标计算（buf_size < 48 时前缀 snprintf 就写越堆块），故在此一次拦下
int clamp_param(const char *name, int value, int lo, int hi)
{
    if (value < lo || value > hi)
    {
        const int fixed = (value < lo) ? lo : hi;
        std::fprintf(stderr, "日志配置 %s = %d 超出范围 [%d, %d]，按 %d 处理\n", name, value, lo, hi, fixed);
        return fixed;
    }
    return value;
}

std::string date_prefix(const struct tm &tm)
{
    char buf[32] = {0};
    snprintf(buf, sizeof(buf), "%d_%02d_%02d_", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

//按天轮转判据：取年月日而非只取 tm_mday，后者跨月同日（8-05→9-05）不触发切换
int day_key(const struct tm &tm)
{
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
} // namespace

Log::Log() = default;

Log::~Log()
{
    shutdown();
}

bool Log::init(const LogConfig &config)
{
    //可重复调用：先停上一个写盘线程、排空并关闭上一个文件；此前直接重新 fopen 会泄漏上一份句柄
    shutdown();

    const int line_cap = clamp_param("buf_size", config.log_buf_size, MIN_LINE_BUF, MAX_LINE_BUF);
    //下限取单行上限：一行必须整体装进一个空块，否则热路径要多一条「一行跨块」兜底，写盘侧也得按行切分
    const int batch_cap =
        clamp_param("batch_buf_size", config.batch_buf_size, MIN_BATCH_BUF_SIZE, MAX_BATCH_BUF_SIZE);
    const int flush_ms =
        clamp_param("flush_interval", config.flush_interval_ms, MIN_FLUSH_INTERVAL_MS, MAX_FLUSH_INTERVAL_MS);
    const int split_lines = clamp_param("split_lines", config.split_lines, 1, INT32_MAX);

    if (config.file.empty())
    {
        std::fprintf(stderr, "日志文件名不能为空\n");
        return false;
    }

    m_line_cap.store(line_cap, std::memory_order_relaxed);
    //写盘线程在创建之前读取它，且线程创建本身构成一次同步，无需额外加锁
    m_flush_interval_ms = flush_ms;

    //tzset 非线程安全，必须在创建写盘线程之前由单线程预热，此后并发取时间只剩读；挪到建线程之后就晚
    tzset();

    time_t t = time(NULL);
    struct tm my_tm = {};
    localtime_r(&t, &my_tm);

    {
        std::lock_guard<std::mutex> guard(m_file_mutex);
        m_dir_name = config.dir;
        if (!m_dir_name.empty() && m_dir_name.back() != '/')
            m_dir_name += '/';
        m_log_name = config.file;
        m_split_lines = split_lines;
        m_day_key = day_key(my_tm);
        m_split_index = 0;
        m_line_count = 0;

        const std::string full = m_dir_name + date_prefix(my_tm) + m_log_name;
        m_fp = fopen(full.c_str(), "a");
        if (m_fp == NULL)
        {
            std::fprintf(stderr, "日志文件 %s 打开失败: %s\n", full.c_str(), strerror(errno));
            return false;
        }
    }

    m_stopping = false;
    m_writer_active = (config.write_mode == 1);

    if (m_writer_active)
    {
        //建池：1 写 + MAX_PENDING_BLOCKS 排队，内存上界 (MAX_PENDING_BLOCKS+1)*batch_buf_size；数组预留容量使热路径此后不分配；同步模式不建池
        const size_t total = MAX_PENDING_BLOCKS + 1;
        m_pool.reserve(total);
        m_pending.reserve(MAX_PENDING_BLOCKS);
        m_free.reserve(total);
        for (size_t i = 0; i < total; ++i)
        {
            auto batch = std::make_unique<Batch>();
            batch->data.reset(new char[batch_cap]);
            batch->cap = batch_cap;
            m_free.push_back(batch.get());
            m_pool.push_back(std::move(batch));
        }
        m_front = m_free.back();
        m_free.pop_back();

        m_writer = std::thread(&Log::writer_loop, this);
    }

    m_ready.store(true, std::memory_order_release);
    return true;
}

void Log::writer_loop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopping)
    {
        //带谓词的 wait_for：notify 早于等待也不丢唤醒；定时醒来看「块没满但等够了」，notify 看「块满了」
        //注意：本工具链 libtsan 对带超时的条件变量等待会记错互斥量持有状态、误报数据竞争与重复加锁，
        //换成不带超时的轮询即归零而实现不变，据此确认是误报。详见 docs/changes/030-log-double-buffer.md
        m_cv.wait_for(lock, std::chrono::milliseconds(m_flush_interval_ms),
                      [this] { return m_stopping || !m_pending.empty(); });
        //定时醒来即「块没满但等够了」：front 里不满一块的内容必须一并交出去，否则会滞留内存到写满一块或进程退出
        retire_front_locked();
        drain_locked(lock);
    }
    //退出前最后一次排空：停机瞬间仍在缓冲里的日志不能丢
    drain_locked(lock);
}

bool Log::retire_front_locked()
{
    //空块不入队：否则其序号被计入，会让 flush 的等待目标提前满足
    if (m_front == NULL || m_front->used == 0)
        return true;

    if (m_free.empty())
        return false;

    m_pending.push_back(m_front);
    m_seq++;
    m_front = m_free.back();
    m_free.pop_back();
    m_front->used = 0;
    m_front->lines = 0;
    return true;
}

void Log::drain_locked(std::unique_lock<std::mutex> &lock)
{
    if (m_pending.empty())
        return;

    //复制一份到局部变量再清空待写队列：块在锁外写；取走与清空在同一临界区内完成，故每块只可能被一个排空者拿到
    const std::vector<Batch *> drained = m_pending;
    m_pending.clear();

    //目标序号须在同一临界区内捕获（本批末块序号）；若改用排空结束时的 m_seq，期间新入队的块会被误报为已落盘
    const uint64_t target = m_seq;
    lock.unlock();

    for (Batch *batch : drained)
        write_batch(*batch);

    lock.lock();
    for (Batch *batch : drained)
    {
        batch->used = 0;
        batch->lines = 0;
        m_free.push_back(batch);
    }
    //取 max：只有真正完成排空的线程推进它，且不让它倒退
    m_drained = std::max(m_drained, target);
    m_flush_cv.notify_all();
}

void Log::write_batch(const Batch &batch)
{
    //只有这里与 rotate_if_needed_locked 碰 m_fp，且都在这把锁之下
    std::lock_guard<std::mutex> guard(m_file_mutex);
    if (m_fp == NULL)
        return; //文件不可用：静默丢弃。日志是辅助设施，不影响主流程

    rotate_if_needed_locked(batch);

    //把丢弃计数作为独立一行写出，让丢失在文件里可见而非无声无息
    const long long dropped = m_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped > 0)
        write_dropped_locked(dropped);

    //整块写 + 立即刷出：fwrite 只把数据交给 stdio 缓冲，进程在其未刷出时退出会丢最后一块；落盘时机已由缓冲池管
    fwrite(batch.data.get(), 1, batch.used, m_fp);
    fflush(m_fp);
    m_line_count += static_cast<long long>(batch.lines);
}

void Log::write_line_direct(const char *data, size_t len, time_t sec)
{
    //同步模式不做缓冲，每行直接落文件；多线程由 m_file_mutex 串行化，故文件顺序即取锁顺序（与缓冲池「单排空者+FIFO」一致）
    std::lock_guard<std::mutex> guard(m_file_mutex);
    if (m_fp == NULL)
        return;

    //借用 Batch 的 lines/first_sec 参与轮转判断（只读这两个字段）
    Batch one;
    one.lines = 1;
    one.first_sec = sec;
    rotate_if_needed_locked(one);

    fwrite(data, 1, len, m_fp);
    fflush(m_fp);
    m_line_count += 1;
}

void Log::rotate_if_needed_locked(const Batch &batch)
{
    struct tm tm_day = {};
    time_t sec = batch.first_sec;
    localtime_r(&sec, &tm_day);

    const int current_day = day_key(tm_day);
    if (current_day != m_day_key)
    {
        //跨天：换到新日期的文件。判据取块内首行的时刻，故只有恰好跨零点的那一块会归到旧日期，偏差上限为一块
        m_day_key = current_day;
        m_split_index = 0;
        m_line_count = 0;
        reopen_locked(date_prefix(tm_day) + m_log_name);
        return;
    }

    //split_lines 是**软**上限：判据是「整块写下去会不会越过」，文件可能多出不到一块的行数；换来写盘侧只做整块 fwrite，无需按行切块
    if (m_line_count > 0 && m_line_count + static_cast<long long>(batch.lines) > m_split_lines)
    {
        m_split_index++;
        m_line_count = 0;
        reopen_locked(date_prefix(tm_day) + m_log_name + "." + std::to_string(m_split_index));
    }
}

void Log::reopen_locked(const std::string &file_name)
{
    //fclose 会一并刷出流缓冲，切换文件前不必再单独 fflush
    if (m_fp != NULL)
        fclose(m_fp);

    const std::string full = m_dir_name + file_name;
    m_fp = fopen(full.c_str(), "a");
    if (m_fp == NULL)
        std::fprintf(stderr, "日志文件 %s 打开失败: %s\n", full.c_str(), strerror(errno));
}

void Log::write_dropped_locked(long long dropped)
{
    //由写盘线程直接写文件，不经 write_log：那条路径可能反过来触发 flush，在自己的线程上等自己会死锁
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    struct tm my_tm = {};
    localtime_r(&now.tv_sec, &my_tm);

    char line[192] = {0};
    const int n = snprintf(line, sizeof(line),
                           "%d-%02d-%02d %02d:%02d:%02d.%06ld [warn]: 已丢弃 %lld 行日志（落盘速度跟不上写入）\n",
                           my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, my_tm.tm_hour, my_tm.tm_min,
                           my_tm.tm_sec, static_cast<long>(now.tv_usec), dropped);
    if (n > 0)
    {
        fwrite(line, 1, static_cast<size_t>(n), m_fp);
        m_line_count += 1;
    }
}

void Log::write_log(int level, const char *format, ...)
{
    //未初始化（关闭日志、init 未执行/失败、已停机）直接返回：不取锁、不格式化、不碰文件；此前 Acceptor 启动期错误路径会在 init 前写日志
    if (!m_ready.load(std::memory_order_acquire))
        return;

    struct timeval now = {0, 0};
    gettimeofday(&now, NULL); //vDSO，不进内核
    //localtime 返回进程内静态缓冲区，会被并发写日志的子 Reactor 线程共写；改用可重入的 localtime_r 写入调用方自己的 tm
    struct tm my_tm = {};
    localtime_r(&now.tv_sec, &my_tm);

    const int cap = m_line_cap.load(std::memory_order_relaxed);

    int n = snprintf(t_line, static_cast<size_t>(cap), "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ", my_tm.tm_year + 1900,
                     my_tm.tm_mon + 1, my_tm.tm_mday, my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec,
                     static_cast<long>(now.tv_usec), level_tag(level));
    if (n < 0)
        n = 0;
    //前缀至多占满末尾的换行位与终止符，保证后面仍留有可用空间
    if (n > cap - 2)
        n = cap - 2;

    va_list valst;
    va_start(valst, format);
    int m = vsnprintf(t_line + n, static_cast<size_t>(cap - n - 1), format, valst);
    va_end(valst);
    if (m < 0)
        m = 0;
    //vsnprintf 截断时返回「本该写入的长度」而非实际写入长度，直接累加会让换行与终止符写到缓冲区之外
    if (m > cap - n - 2)
        m = cap - n - 2;

    t_line[n + m] = '\n';
    t_line[n + m + 1] = '\0';
    const size_t len = static_cast<size_t>(n + m + 1); //含换行，不含终止符

    bool need_wake = false;
    bool dropped = false;
    bool sync_mode = false;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        //m_writer_active 只由 init/shutdown 改写（分别发生在子线程尚未创建、已 join 时），不与 write_log 并发，故锁内读它安全
        sync_mode = !m_writer_active;

        if (!sync_mode)
        {
            if (m_front == NULL)
                return;

            //单行须整体落在同一块里：块是落盘最小单位，行跨块就要求写盘侧按行切分；batch_buf_size 下限已夹到单行上限，空块一定装得下
            if (m_front->used + len > m_front->cap)
            {
                if (retire_front_locked())
                {
                    need_wake = true;
                }
                else
                {
                    //背压：池中的块都在等落盘。不同步写盘——此前队列满时由调用线程直接落盘、把磁盘写压到 Reactor 线程；改为丢弃并计数，由写盘线程写进日志显式化
                    m_dropped.fetch_add(1, std::memory_order_relaxed);
                    dropped = true;
                }
            }

            if (!dropped)
            {
                if (m_front->used == 0)
                    m_front->first_sec = now.tv_sec;
                memcpy(m_front->data.get() + m_front->used, t_line, len);
                m_front->used += len;
                m_front->lines += 1;
            }
        }
    }

    if (sync_mode)
    {
        write_line_direct(t_line, len, now.tv_sec);
        return;
    }

    //唤醒放在锁外，且只在换块时做。逐行唤醒会把刚省下的系统调用又加回来：写盘线程多半在睡，这次唤醒是一次 futex
    if (need_wake)
        m_cv.notify_all();
}

void Log::flush(void)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    //同步模式每行都已直接落到文件，无待排空内容；未初始化或已停机同理
    if (!m_writer_active || m_front == NULL)
        return;

    if (!m_ready.load(std::memory_order_acquire))
        return; //停机中：写盘线程退出前会做最后一次排空，这里不与它争

    //反复「唤醒—等待—再试」直到 front 内容真的被交出去并落盘：池无空闲块时 retire 会失败，只等一次就返回会漏掉还留在 front 里的行
    while (m_front->used > 0)
    {
        const bool retired = retire_front_locked();
        const uint64_t target = m_seq;
        m_cv.notify_all(); //定时未到也叫醒它
        m_flush_cv.wait(lock, [this, target, retired]
                        { return m_drained >= target && (retired || !m_free.empty()); });
    }
}

void Log::shutdown()
{
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        //先关门：此后 write_log 直接丢弃，不再碰池与文件
        m_ready.store(false, std::memory_order_release);
        m_stopping = true;
        m_cv.notify_all();
    }

    if (m_writer.joinable())
        m_writer.join(); //写盘线程退出前会做最后一次排空

    //写盘线程已不在，但从它最后一次排空到 join 返回之间调用方可能又写了几行，由调用线程再排空一次；
    //等待 flush 的线程也会因序号推进被唤醒（drain_locked 取「取走待写队列那一刻」的序号，停机只会让它更早满足）
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_writer_active = false;
        //先排空再把 front 交出去：池可能已被占满，直接 retire 会失败，那些行就再也写不出去了
        drain_locked(lock);
        retire_front_locked();
        drain_locked(lock);
    }

    {
        std::lock_guard<std::mutex> guard(m_file_mutex);
        if (m_fp != NULL)
        {
            fclose(m_fp);
            m_fp = NULL;
        }
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pool.clear();
        m_pending.clear();
        m_free.clear();
        m_front = NULL;
        m_seq = 0;
        m_drained = 0;
        m_dropped.store(0, std::memory_order_relaxed);
    }
}
