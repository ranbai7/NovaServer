#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <cstddef>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../CGImysql/sql_connection_pool.h"
#include "../lock/locker.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    //最长固定页面路径是 "/Upload-Success.html"（20 字节），根目录长度须为它留余量，否则拼进 m_real_file 会被截掉
    static const int LONGEST_PAGE_PATH_LEN = 20;
    static const int MAX_ROOT_DIR_LEN = FILENAME_LEN - LONGEST_PAGE_PATH_LEN - 1;
    //两块缓冲区按需扩容，下列常量为初始大小，上限见 MAX_*_SIZE；请求体超限返回 413，不再像此前那样直接关闭连接
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    static const int MAX_REQUEST_SIZE = 8 * 1024 * 1024;
    static const int MAX_RESPONSE_SIZE = 64 * 1024;
    static const int MAX_HEADER_SIZE = 8 * 1024;        //请求头部（不含请求行与请求体）的长度上限
    static const int MAX_UPLOAD_SIZE = 4 * 1024 * 1024; //单个上传文件的体积上限，须小于 MAX_REQUEST_SIZE
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        REQUEST_TOO_LARGE,
        REQUEST_HEADER_TOO_LARGE,
        METHOD_NOT_IMPLEMENTED,
        UNSUPPORTED_MEDIA_TYPE,
        EXPECTATION_FAILED,
        DYNAMIC_CONTENT,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn()
    {
        //未注入通知器时也要可调用：缺省退化为「什么也不做」，而不是在调用点抛 bad_function_call
        m_want_read = [] {};
        m_want_write = [] {};

        //必须先置空：reset() 首步的 unmap() 要读 m_file_address；连接对象复用堆内存，残留旧地址会再次 munmap，解除别人的映射，表现为无从解释的堆破坏
        m_url = nullptr;
        m_version = nullptr;
        m_host = nullptr;
        m_string = nullptr;
        m_file_address = nullptr;
        m_file_stat = {};
    }
    ~http_conn() {}

public:
    //事件注册权归连接的所有者：协议层只表达「接下来关心什么」，不直接操作 epoll；状态机里的读写意图留在这里最自然，改动也只有几处调用点
    using EventCallback = std::function<void()>;
    void set_event_notifier(EventCallback want_read, EventCallback want_write);

    void init(int sockfd, const sockaddr_in &addr, const char *root, int TRIGMode, int close_log,
              connection_pool *connPool);
    //返回 false 表示该连接应当关闭，释放动作交给调用方的唯一出口
    bool process();
    bool read_once();
    bool write();
    const sockaddr_in *get_address() const { return &m_address; }
    //加载用户表到进程内缓存，与实例无关，启动期一次性调用；日志开关须作参数传入，静态函数读不到连接成员
    static void initmysql_result(connection_pool *connPool, int close_log);
    //释放文件映射。连接在任何时刻关闭都要调用：映射可能建立在尚未写出响应的请求上，那条路径不经 write() 里的释放
    void unmap();
    //是否还有未完成的工作：已收到未处理的请求或已生成未发完的响应；对端半关闭时据此判断能否立即关闭
    bool has_pending_work() const { return m_read_idx > 0 || bytes_to_send > 0; }

private:
    void reset();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    //回应 100 Continue，告知客户端可以开始发送请求体
    void send_continue();
    HTTP_CODE do_request();
    //把「根目录 + 相对路径」写入 m_real_file；超出容量返回 false，调用方应判为错误请求：截断的路径并非请求目标
    bool set_real_file(const char *relative_path);
    char *get_line() { return m_read_buf.data() + m_start_line; };
    LINE_STATUS parse_line();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type(const char *type);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

    //缓冲区扩容：读缓冲不足时按几何级数增长，并把指向它的成员一并重新指向
    bool ensure_read_space();
    void grow_read_buffer(size_t size);
    bool grow_write_buffer();

    HTTP_CODE parse_multipart_content();             // 解析 multipart 请求体
    HTTP_CODE check_upload_name_early();             // 请求体未收完时的上传名提前判定
    bool save_uploaded_file();                       // 保存文件到磁盘
    HTTP_CODE serve_uploaded_file(const char *name); // 返回已上传的文件
    HTTP_CODE build_upload_list();                   // 生成已上传文件的列表页
    void init_file_upload_state();                   // 重置上传状态

public:
    MYSQL *mysql;
    int m_state; //读为0, 写为1

private:
    //登录与注册要从连接池取连接；池的生存期覆盖全部请求，故这里只持不具所有权的指针，给默认值是为了未走 init 的路径不读到未初始化取值
    connection_pool *m_connPool = nullptr;

    int m_sockfd;
    sockaddr_in m_address;
    std::vector<char> m_read_buf;
    long m_read_idx;
    long m_checked_idx;
    long m_body_start;
    int m_start_line;
    std::vector<char> m_write_buf;
    int m_write_idx;
    int m_header_end;          //响应头结束（即正文起始）在写缓冲区中的位置
    bool m_oversized;          //请求体已超过 MAX_REQUEST_SIZE
    bool m_has_content_length; //是否已出现过 Content-Length 头
    bool m_expect_continue;    //请求带 Expect: 100-continue，需在收请求体前回应 100
    //本次是否已完整解析出一个请求；置位后读缓冲剩余字节才属于下一个请求（HTTP 管线化），复位时才敢保留
    bool m_request_parsed = false;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    const char *doc_root; //指向服务器持有的根目录，不拥有

    int m_TRIGMode;
    int m_close_log;

    //「接下来关心什么事件」由这两处表达，实际注册由所有者完成
    EventCallback m_want_read;
    EventCallback m_want_write;

    // ========== 文件上传新增成员变量 ==========
    bool m_is_file_upload;      // 是否为文件上传请求
    bool m_is_upload_download;  // 是否为已上传文件的下载响应
    std::string m_boundary;     // multipart boundary（含 "--" 前缀）
    std::string m_file_name;    // 上传的文件名
    std::string m_file_content; // 文件内容（仅小文件）

    // 动态生成的响应正文（目前用于上传列表页）
    std::string m_inline_body;
    std::string m_inline_content_type;
};

#endif
