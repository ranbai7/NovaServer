#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <cstddef>
#include <errno.h>
#include <fcntl.h>
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
#include "../timer/lst_timer.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    //固定页面中最长的路径是上传成功后的跳转页 "/Upload-Success.html"（20 字节）。
    //根目录长度须为它留出余量，否则这些页面拼进 m_real_file 时会被截掉
    static const int LONGEST_PAGE_PATH_LEN = 20;
    static const int MAX_ROOT_DIR_LEN = FILENAME_LEN - LONGEST_PAGE_PATH_LEN - 1;
    //两个缓冲区按需扩容：下面两个常量为初始大小，上限见 MAX_*_SIZE。
    //请求体超出上限时返回 413，而不是像此前那样把连接直接关掉
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
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    const sockaddr_in *get_address() const { return &m_address; }
    //重新激活 epoll 关注，返回 false 表示该连接已无待办、可以关闭。
    //EPOLLONESHOT 下每次事件后关注都会失效，若某次事件由不触发读写的分支
    //消费掉（例如对端半关闭），必须重新激活，否则该连接再无事件可达
    bool rearm_epoll();
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    //回应 100 Continue，告知客户端可以开始发送请求体
    void send_continue();
    HTTP_CODE do_request();
    //把「根目录 + 相对路径」写入 m_real_file。总长超出容量时返回 false，
    //调用方应判为错误请求：被截断的路径会指向并非请求目标的文件
    bool set_real_file(const char *relative_path);
    char *get_line() { return m_read_buf.data() + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
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

    // 新增：文件上传相关方法
    HTTP_CODE parse_multipart_content();             // 解析 multipart 请求体
    HTTP_CODE check_upload_name_early();             // 请求体未收完时的上传名提前判定
    bool save_uploaded_file();                       // 保存文件到磁盘
    HTTP_CODE serve_uploaded_file(const char *name); // 返回已上传的文件
    HTTP_CODE build_upload_list();                   // 生成已上传文件的列表页
    void init_file_upload_state();                   // 重置上传状态

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state; //读为0, 写为1

private:
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
    char *doc_root;

    int m_TRIGMode;
    int m_close_log;

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
