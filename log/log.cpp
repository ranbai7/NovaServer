#include "log.h"
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
    //这些指针在 init 之前也可能被析构函数访问，先置空避免读到未初始化的取值
    m_fp = nullptr;
    m_buf = nullptr;
    m_log_queue = nullptr;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
    delete[] m_buf;
    delete m_log_queue;
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        delete m_log_queue;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    //init 可能被多次调用，先释放上一个缓冲区，否则每次调用都会泄漏一份
    delete[] m_buf;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    //时区状态是进程级的全局数据，首次取本地时间会触发一次初始化（tzset），
    //而那一步不是线程安全的。init 在启动阶段由单线程调用，先在这里预热一次，
    //此后各子 Reactor 线程并发取时间就只剩读操作
    tzset();

    time_t t = time(NULL);
    struct tm my_tm = {};
    localtime_r(&t, &my_tm);

    const char *p = strrchr(file_name, '/');
    char log_full_name[512] = {0};

    if (p == NULL)
    {
        snprintf(log_full_name, sizeof(log_full_name), "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                 my_tm.tm_mday, file_name);
    }
    else
    {
        //与下面的 dir_name 同样限长：文件名来自配置，可能远超 log_name 的容量
        size_t name_len = strlen(p + 1);
        if (name_len > sizeof(log_name) - 1)
            name_len = sizeof(log_name) - 1;
        memcpy(log_name, p + 1, name_len);
        log_name[name_len] = '\0';
        //仅复制目录部分（含末尾的 '/'），并保证不越界且以 '\0' 结尾
        size_t dir_len = static_cast<size_t>(p - file_name) + 1;
        if (dir_len > sizeof(dir_name) - 1)
            dir_len = sizeof(dir_name) - 1;
        memcpy(dir_name, file_name, dir_len);
        dir_name[dir_len] = '\0';
        snprintf(log_full_name, sizeof(log_full_name), "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900,
                 my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    //localtime 返回指向进程内静态缓冲区的指针，而写日志会被多个子 Reactor 线程
    //并发调用，它们会同时读写那份静态数据；改用可重入的 localtime_r，
    //结果写入调用方自己的 tm
    struct tm my_tm = {};
    localtime_r(&t, &my_tm);
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();

    //init 失败时文件指针为空。日志是辅助设施，此时应静默丢弃而不是影响主流程。
    //这个判断必须在锁内：m_fp 会被下面的日志轮转改写（fclose 之后 fopen 之前
    //它还一度是悬垂的），放在锁外读会与那一步构成数据竞争
    if (m_fp == nullptr)
    {
        m_mutex.unlock();
        return;
    }

    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        char new_log[512] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[32] = {0};

        snprintf(tail, sizeof(tail), "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, sizeof(new_log), "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, sizeof(new_log), "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ", my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                     my_tm.tm_mday, my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    if (n < 0)
        n = 0;
    //前缀至多占满缓冲区末尾的换行位与终止符，保证后面仍留有可用空间
    if (n > m_log_buf_size - 2)
        n = m_log_buf_size - 2;

    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    if (m < 0)
        m = 0;
    //vsnprintf 在截断时返回「本该写入的长度」而非实际写入长度，
    //若直接按其累加，随后的换行与终止符会写到缓冲区之外
    if (m > m_log_buf_size - n - 2)
        m = m_log_buf_size - n - 2;

    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
