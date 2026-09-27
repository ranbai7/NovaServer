#include "http_conn.h"
#include "../auth/password_hash.h"
#include "url_codec.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mysql/mysql.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_413_title = "Request Entity Too Large";
const char *error_413_form = "The request is larger than this server is willing to process.\n";
const char *error_431_title = "Request Header Fields Too Large";
const char *error_431_form = "The request headers are larger than this server is willing to process.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";
const char *error_501_title = "Not Implemented";
const char *error_501_form = "This server does not implement the requested method.\n";
const char *error_415_title = "Unsupported Media Type";
const char *error_415_form = "The uploaded file type is not accepted by this server.\n";
const char *error_417_title = "Expectation Failed";
const char *error_417_form = "The expectation given in the Expect header cannot be met.\n";

//服务端实现之外但属于 HTTP 规范的请求方法。它们与无法识别的记号需要区分开：
//前者应当回 501，后者是语法错误
bool is_known_method(const char *method)
{
    static const char *kMethods[] = {"PUT", "DELETE", "TRACE", "OPTIONS", "CONNECT", "PATCH"};
    for (const char *known : kMethods)
    {
        if (strcasecmp(method, known) == 0)
            return true;
    }
    return false;
}

//解析 Content-Length 的取值。只接受非空的十进制数字串：
//atol 会把 "abc" 静默当作 0、把 "12abc" 当作 12，两种都会让长度与实际不符
bool parse_content_length(const char *text, long &out)
{
    if (text == nullptr || *text == '\0')
        return false;

    long value = 0;
    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10 + (*p - '0');
        if (value > http_conn::MAX_REQUEST_SIZE)
        {
            out = value; //交给调用方判为超限
            return true;
        }
    }
    out = value;
    return true;
}

//上传目录。与站点根目录分开：站点根目录提供站点自身的内容，上传目录的内容来自
//用户，两者的访问规则不同（后者一律按附件下载）
const char *kUploadDir = "./upload";

//允许上传的扩展名白名单。上传内容会落盘并通过 HTTP 提供，因此限定在明确的类型上：
//不接受超文本与脚本（.html/.svg/.js），它们在浏览器中可携带并执行脚本
const char *kAllowedUploadExt[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp",
                                   ".mp4", ".webm", ".mp3", ".txt", ".pdf", ".zip"};

bool is_allowed_upload_name(const std::string &name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;

    const std::string ext = name.substr(dot);
    for (const char *allowed : kAllowedUploadExt)
    {
        if (strcasecmp(ext.c_str(), allowed) == 0)
            return true;
    }
    return false;
}

//响应头中的取值不能带控制字符与引号，否则可注入额外头部或截断取值
std::string sanitize_header_value(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        out.push_back((uc < 0x20 || uc == 0x7f || c == '"' || c == '\\') ? '_' : c);
    }
    return out;
}

//列表页中的文件名来自用户，必须转义，否则文件名里的标签会被当作页面结构
std::string html_escape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

//链接中的文件名同样需要转义，保证特殊字符不会改变链接指向
std::string url_encode_segment(const std::string &text)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '.' || c == '-' || c == '_' || c == '~';
        if (unreserved)
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// 新增：根据文件扩展名返回 MIME 类型
static const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0)
        return "image/png";
    if (strcasecmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcasecmp(ext, ".mp4") == 0)
        return "video/mp4";
    if (strcasecmp(ext, ".avi") == 0)
        return "video/x-msvideo";
    if (strcasecmp(ext, ".txt") == 0)
        return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".json") == 0)
        return "application/json";
    if (strcasecmp(ext, ".ico") == 0)
        return "image/x-icon";
    return "application/octet-stream";
}

namespace
{
//已注册用户的用户名与口令。该表被多个工作线程并发读写，
//因此全部访问都收敛到下面两个加锁封装中，避免出现「写侧加锁、读侧裸读」的不对称同步
std::map<std::string, std::string> g_users;
locker g_users_lock;

//查询用户口令，找到时写入 passwd 并返回 true
bool lookup_user(const std::string &name, std::string &passwd)
{
    g_users_lock.lock();
    auto it = g_users.find(name);
    bool found = (it != g_users.end());
    if (found)
        passwd = it->second;
    g_users_lock.unlock();
    return found;
}

//用变换后的路径覆盖读缓冲区中的原文，返回原指针以便链式书写。
//解码与规范化都只会缩短路径，因此就地写回不会超出原占用的空间
char *overwrite_url(char *url, const std::string &replacement)
{
    memcpy(url, replacement.data(), replacement.size());
    url[replacement.size()] = '\0';
    return url;
}

//解析 application/x-www-form-urlencoded 请求体：字段以 '&' 分隔，
//键与值以第一个 '=' 分隔。字段缺失、为空或任意长都不会越界，
//也不依赖字段名、顺序或长度等任何固定假设
std::map<std::string, std::string> parse_form_body(const char *body, long length)
{
    std::map<std::string, std::string> params;
    if (body == nullptr || length <= 0)
        return params;

    const std::string data(body, static_cast<size_t>(length));
    size_t pos = 0;
    while (pos <= data.size())
    {
        size_t amp = data.find('&', pos);
        if (amp == std::string::npos)
            amp = data.size();

        const std::string field = data.substr(pos, amp - pos);
        const size_t eq = field.find('=');
        if (eq != std::string::npos)
        {
            const std::string key = url_codec::decode(field.substr(0, eq), true);
            if (!key.empty())
                params[key] = url_codec::decode(field.substr(eq + 1), true);
        }

        if (amp == data.size())
            break;
        pos = amp + 1;
    }
    return params;
}

//启动期致命错误：日志开关可能被关闭，因此始终同时写标准错误
[[noreturn]] void fatal(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::exit(EXIT_FAILURE);
}

//预处理语句的字符串参数绑定。
//MYSQL_BIND 中的 length 是指针，直接指向临时变量会留下悬垂指针，
//因此把长度变量与绑定数组放在同一处持有
class StringBinder
{
public:
    explicit StringBinder(size_t count) : m_binds(count), m_lengths(count, 0) {}

    void set(size_t index, const std::string &value)
    {
        m_lengths[index] = value.size();
        m_binds[index].buffer_type = MYSQL_TYPE_STRING;
        m_binds[index].buffer = const_cast<char *>(value.data());
        m_binds[index].buffer_length = m_lengths[index];
        m_binds[index].length = &m_lengths[index];
    }

    MYSQL_BIND *data() { return m_binds.data(); }

private:
    std::vector<MYSQL_BIND> m_binds;
    std::vector<unsigned long> m_lengths;
};

//执行一条带两个字符串参数的写语句，参数不参与 SQL 文本拼接
bool execute_two_params(MYSQL *conn, const char *sql, const std::string &first, const std::string &second)
{
    MYSQL_STMT *stmt = mysql_stmt_init(conn);
    if (stmt == nullptr)
        return false;

    bool ok = false;
    if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(std::strlen(sql))) == 0)
    {
        StringBinder binder(2);
        binder.set(0, first);
        binder.set(1, second);
        if (mysql_stmt_bind_param(stmt, binder.data()) == 0)
            ok = (mysql_stmt_execute(stmt) == 0);
    }
    mysql_stmt_close(stmt);
    return ok;
}

//哈希串比明文长得多，旧表的 char(50) 存不下。列宽不足时先加宽，
//否则失败会推迟到注册或迁移写入时才暴露
const long kPasswordColumnLength = 255;

void ensure_password_column(MYSQL *conn)
{
    static const char kQuery[] = "SELECT CHARACTER_MAXIMUM_LENGTH FROM information_schema.COLUMNS "
                                 "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'user' "
                                 "AND COLUMN_NAME = 'passwd'";
    if (mysql_query(conn, kQuery) != 0)
        fatal("查询 user.passwd 列定义失败: %s", mysql_error(conn));

    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr)
        fatal("读取 user.passwd 列定义失败: %s", mysql_error(conn));

    MYSQL_ROW row = mysql_fetch_row(result);
    const bool too_narrow =
        (row != nullptr && row[0] != nullptr && std::strtol(row[0], nullptr, 10) < kPasswordColumnLength);
    mysql_free_result(result);

    if (!too_narrow)
        return;

    static const char kAlter[] = "ALTER TABLE user MODIFY passwd VARCHAR(255)";
    if (mysql_query(conn, kAlter) != 0)
        fatal("加宽 user.passwd 列失败: %s", mysql_error(conn));

    //此处不使用 LOG_WARN 宏：它展开后依赖调用作用域的 m_close_log 成员，本函数并无此成员
    std::fprintf(stderr, "user.passwd 列已加宽至 VARCHAR(%ld) 以容纳口令哈希\n", kPasswordColumnLength);
}

enum class RegisterResult
{
    kInserted,
    kDuplicate,
    kFailed
};

//注册用户：在锁内完成「查重 → 写库 → 更新内存」的完整序列，
//使判重与插入之间不存在竞态窗口
RegisterResult register_user(const std::string &name, const std::string &password, MYSQL *conn)
{
    g_users_lock.lock();
    if (g_users.find(name) != g_users.end())
    {
        g_users_lock.unlock();
        return RegisterResult::kDuplicate;
    }

    //库中只保存加盐哈希，明文不落库
    const std::string encoded = password_hash::encode(password);
    const bool inserted =
        !encoded.empty() && execute_two_params(conn, "INSERT INTO user(username, passwd) VALUES(?, ?)", name, encoded);

    if (inserted)
        g_users[name] = encoded;
    g_users_lock.unlock();
    return inserted ? RegisterResult::kInserted : RegisterResult::kFailed;
}
} // namespace

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //确保 passwd 列足以容纳哈希串
    ensure_password_column(mysql);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        return;
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        LOG_ERROR("mysql_store_result error:%s\n", mysql_error(mysql));
        return;
    }

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    g_users_lock.lock();
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        const std::string name(row[0] != nullptr ? row[0] : "");
        std::string stored(row[1] != nullptr ? row[1] : "");
        if (name.empty())
            continue;

        //历史遗留的明文记录在此升级为哈希，使库中不再留存明文。
        //升级失败必须终止启动：否则该账号会带着明文留在内存中而校验永远不通过，
        //表现为「口令正确却登录失败」，比启动失败更难定位
        if (!password_hash::is_encoded(stored))
        {
            const std::string encoded = password_hash::encode(stored);
            if (encoded.empty())
                fatal("为用户 %s 生成口令哈希失败", name.c_str());
            if (!execute_two_params(mysql, "UPDATE user SET passwd = ? WHERE username = ?", encoded, name))
                fatal("升级用户 %s 的口令失败: %s", name.c_str(), mysql_error(mysql));

            LOG_INFO("已将用户 %s 的明文口令升级为加盐哈希", name.c_str());
            stored = encoded;
        }
        g_users[name] = stored;
    }
    g_users_lock.unlock();

    //结果集由调用方负责释放，否则每次加载用户表都会泄漏一份
    mysql_free_result(result);
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log,
                     const std::string &user, const std::string &passwd, const std::string &sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    //成员必须先于 addfd 赋值：addfd 要读取 m_TRIGMode 决定 connfd 的触发模式，
    //若在赋值前调用，注册时读到的是未初始化的值，-m 参数中的连接侧组合因而从未生效
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    snprintf(sql_user, sizeof(sql_user), "%s", user.c_str());
    snprintf(sql_passwd, sizeof(sql_passwd), "%s", passwd.c_str());
    snprintf(sql_name, sizeof(sql_name), "%s", sqlname.c_str());

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    //本服务端只接受 HTTP/1.1，而该版本默认保持连接：只有请求显式给出
    //Connection: close 时才会置假（见 parse_headers）
    m_linger = true;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_body_start = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    //指向请求体与文件映射的指针必须显式复位：连接对象会被复用，
    //残留的上次取值会让 unmap 与判空逻辑作用于已失效的地址
    m_string = nullptr;
    m_file_address = nullptr;
    m_iv_count = 0;

    // 新增：重置文件上传状态
    init_file_upload_state();

    m_oversized = false;
    m_has_content_length = false;
    m_expect_continue = false;
    m_header_end = 0;
    //连接复用时不保留上次为超大请求扩容出来的缓冲，避免大请求之后内存被长期占用
    if (m_read_buf.capacity() > static_cast<size_t>(READ_BUFFER_SIZE) * 4)
        std::vector<char>().swap(m_read_buf);
    m_read_buf.resize(READ_BUFFER_SIZE);
    m_write_buf.resize(WRITE_BUFFER_SIZE);
    memset(m_read_buf.data(), '\0', m_read_buf.size());
    memset(m_write_buf.data(), '\0', m_write_buf.size());
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//扩容读缓冲区。重分配之后缓冲区地址会变化，此前解析出的指针随之失效，
//因此在这里把它们按同样的偏移重新指向新地址。
//注意：凡是指向读缓冲区的成员都必须在下面一并调整
void http_conn::grow_read_buffer(size_t size)
{
    const char *old_data = m_read_buf.data();
    m_read_buf.resize(size);

    const ptrdiff_t delta = m_read_buf.data() - old_data;
    if (delta == 0)
        return;

    if (m_url != nullptr)
        m_url += delta;
    if (m_version != nullptr)
        m_version += delta;
    if (m_host != nullptr)
        m_host += delta;
    if (m_string != nullptr)
        m_string += delta;
}

//确认读缓冲区仍有空余：不足则扩容，达到请求体积上限则标记超限
bool http_conn::ensure_read_space()
{
    if (static_cast<size_t>(m_read_idx) < m_read_buf.size())
        return true;

    const size_t grown = m_read_buf.size() * 2;
    if (grown > static_cast<size_t>(MAX_REQUEST_SIZE))
    {
        //请求体超过服务端允许的上限。这里不直接关闭连接，
        //而是置位后交由 process_read / process_write 回应 413
        m_oversized = true;
        return false;
    }

    grow_read_buffer(grown);
    return true;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    //已判定超限：不再读取，等待 413 发出
    if (m_oversized)
        return true;

    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        //缓冲区已满且不能继续扩容，交由 process_read 返回 413
        if (!ensure_read_space())
            return true;

        bytes_read =
            recv(m_sockfd, m_read_buf.data() + m_read_idx, m_read_buf.size() - static_cast<size_t>(m_read_idx), 0);

        //先判返回值再累加读索引：负数直接累加会破坏索引，后续解析将读到错误位置
        if (bytes_read < 0)
        {
            //非阻塞下 EAGAIN 表示此刻无数据可读，连接仍然有效；
            //其余负值才是真正的读取出错
            return (errno == EAGAIN || errno == EWOULDBLOCK);
        }
        if (bytes_read == 0)
        {
            return false; //对端已关闭
        }

        m_read_idx += bytes_read;
        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            if (!ensure_read_space())
                break; //已超限，停止读取

            bytes_read =
                recv(m_sockfd, m_read_buf.data() + m_read_idx, m_read_buf.size() - static_cast<size_t>(m_read_idx), 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else if (strcasecmp(method, "HEAD") == 0)
    {
        //HEAD 的响应头与 GET 一致，只是不发送正文
        m_method = HEAD;
    }
    else if (is_known_method(method))
        return METHOD_NOT_IMPLEMENTED; //规范定义但本服务端未实现
    else
        return BAD_REQUEST; //不是合法的请求方法
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    //解码与规范化的顺序不可颠倒：%2e%2e%2f 解码后才是 '..' 段，
    //若先规范化后解码，编码形式就能绕过规范化里的全部判断
    const std::string decoded = url_codec::decode(m_url, false);
    //%00 解码后是字符串结束符。路径随后要按 C 字符串参与拼接与判等，
    //含结束符的内容会在那里被截断，使实际处理的路径短于这里的判断对象
    if (decoded.find('\0') != std::string::npos)
        return BAD_REQUEST;
    m_url = overwrite_url(m_url, decoded);

    const std::string normalized = url_codec::normalize_path(m_url);
    if (normalized.empty())
        return BAD_REQUEST; //路径试图越过根目录
    m_url = overwrite_url(m_url, normalized);

    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //头部整体长度上限。请求行的结束位置即头部的起点，因此以解析游标衡量
    if (m_checked_idx > MAX_HEADER_SIZE)
        return REQUEST_HEADER_TOO_LARGE;

    if (text[0] == '\0')
    {
        //本服务端只接受 HTTP/1.1，而该版本要求请求必须携带 Host
        if (m_host == nullptr)
            return BAD_REQUEST;

        if (m_content_length != 0)
        {
            //请求体的起点在此处记录一次。解析游标 m_checked_idx 会随每次按行扫描
            //向后移动，不能用它推算请求体位置
            m_body_start = m_checked_idx;
            m_check_state = CHECK_STATE_CONTENT;

            //客户端在等 100 Continue 才肯发送请求体，因此必须在等待之前回应。
            //放到这里而不是解析到 Expect 头时就发：Content-Length 也已校验完毕
            //（超限的请求在解析那个头时即已返回 413），此时才是「确定要收这个体」
            if (m_expect_continue)
                send_continue();

            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        //本服务端只接受 HTTP/1.1，而该版本默认保持连接：m_linger 的基准值已是真，
        //这里只需处理显式关闭。此前只在收到 keep-alive 时才置真，
        //于是不带该头的客户端（curl、wrk 等多数实现）每个请求都要重连
        if (strcasecmp(text, "close") == 0)
        {
            m_linger = false;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");

        //重复的 Content-Length 会让长度出现两种解释，是请求走私的常见手法，直接拒绝
        if (m_has_content_length)
            return BAD_REQUEST;
        m_has_content_length = true;

        if (!parse_content_length(text, m_content_length))
            return BAD_REQUEST;
        //长度本身已超过服务端上限时无需再传输请求体
        if (m_content_length > MAX_REQUEST_SIZE)
            return REQUEST_TOO_LARGE;
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    //新增：
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");
        std::string content_type(text);
        if (content_type.find("multipart/form-data") != std::string::npos)
        {
            m_is_file_upload = true;
            size_t pos = content_type.find("boundary=");
            if (pos != std::string::npos)
            {
                m_boundary = "--";
                m_boundary += content_type.substr(pos + 9);
                // 去除 boundary 可能有的双引号
                if (m_boundary.size() > 2 && m_boundary[2] == '"' && m_boundary.back() == '"')
                {
                    m_boundary = "--" + m_boundary.substr(3, m_boundary.size() - 4);
                }
            }
        }
    }

    else if (strncasecmp(text, "Expect:", 7) == 0)
    {
        text += 7;
        text += strspn(text, " \t");

        //HTTP/1.1 中唯一定义的期望是 100-continue：客户端在发送大体积请求体前
        //先征求许可。不理会它并不会出错，但客户端要一直等到自身的期望超时
        //才会把请求体发出来，这段时间是纯损耗
        if (strcasecmp(text, "100-continue") == 0)
        {
            m_expect_continue = true;
        }
        else
        {
            //其余期望值本服务端无法满足，按规范回 417
            return EXPECTATION_FAILED;
        }
    }

    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//回应 100 Continue。这一行不参与写缓冲的组装：写缓冲承载的是最终响应，
//而 100 是请求处理中途的中间响应，写完即被后续的最终响应覆盖
void http_conn::send_continue()
{
    static const char kContinueResponse[] = "HTTP/1.1 100 Continue\r\n\r\n";
    const ssize_t written = send(m_sockfd, kContinueResponse, sizeof(kContinueResponse) - 1, 0);

    //非阻塞套接字上 25 字节写不下意味着发送缓冲已满，此时客户端通常也尚未开始
    //发送请求体，数据会随后续写入排空。这种情况不值得中断整个请求
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        LOG_WARN("send 100 continue failed, errno:%d", errno);
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content([[maybe_unused]] char *text)
{
    //以 m_body_start 而非 m_checked_idx 为基准：后者会被 parse_line 推到读索引处
    if (m_read_idx >= (m_content_length + m_body_start))
    {
        //请求体由 Content-Length 定长界定，消费方都带长度读取（m_string 与
        //m_content_length 成对使用），因此不在此处补写结束符——那个位置属于
        //紧随其后的字节，写下结束符会把流水线中的下一个请求破坏掉
        m_string = m_read_buf.data() + m_body_start;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    //读取阶段已判定请求超出体积上限，无需再解析
    if (m_oversized)
        return REQUEST_TOO_LARGE;

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while (true)
    {
        //请求体由 Content-Length 定长界定，不按行切分，因此内容阶段跳过 parse_line：
        //对请求体调用它会在找不到行结束符时把 m_checked_idx 推到读索引处，
        //使解析游标随每次读取向后漂移
        if (m_check_state != CHECK_STATE_CONTENT)
        {
            line_status = parse_line();
            if (line_status == LINE_OPEN)
                break; //行未收完
            if (line_status == LINE_BAD)
                return BAD_REQUEST;

            text = get_line();
            m_start_line = m_checked_idx;
            LOG_INFO("%s", text);
        }

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            //请求行一旦出结果即为最终结果。只判 BAD_REQUEST 是不够的：
            //方法未实现等其它取值会被当作「还没解析完」，
            //从而把下一行（例如 Host 头）当成请求行继续解析
            if (ret != NO_REQUEST)
                return ret;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            else if (ret != NO_REQUEST)
            {
                return ret; //头部过长或长度字段已超限等，直接作为最终结果
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            //请求体尚未收完，等待后续数据
            return NO_REQUEST;
        }
        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

bool http_conn::set_real_file(const char *relative_path)
{
    const int written = snprintf(m_real_file, sizeof(m_real_file), "%s%s", doc_root, relative_path);
    return written > 0 && written < static_cast<int>(sizeof(m_real_file));
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // ========== 文件上传处理 ==========
    if (m_method == POST && m_is_file_upload && strncmp(m_url, "/upload", 7) == 0)
    {
        HTTP_CODE ret = parse_multipart_content();
        if (ret != GET_REQUEST)
            return BAD_REQUEST;

        //点号开头的文件在列表页会被跳过，若在此放行就会出现「上传成功却看不见」。
        //两侧统一为拒绝，列表页的跳过逻辑保持不变
        if (!m_file_name.empty() && m_file_name[0] == '.')
            return BAD_REQUEST;

        //扩展名白名单与体积上限在落盘之前校验
        if (!is_allowed_upload_name(m_file_name))
            return UNSUPPORTED_MEDIA_TYPE;
        if (static_cast<long>(m_file_content.size()) > MAX_UPLOAD_SIZE)
            return REQUEST_TOO_LARGE;

        if (!save_uploaded_file())
            return BAD_REQUEST;

        strcpy(m_url, "/Upload-Success.html");
    }
    // =================================

    // ========== 已上传文件的访问 ==========
    //"GET /upload" 给出列表页，"GET /upload/<名称>" 返回对应文件，
    //使上传后的内容可以通过 HTTP 取回（此前文件写入 ./upload/，
    //而静态资源根目录是 ./root/，上传后无从访问）
    if (m_method != POST && strncmp(m_url, "/upload", 7) == 0)
    {
        if (m_url[7] == '\0')
            return build_upload_list();
        if (m_url[7] == '/')
            return serve_uploaded_file(m_url + 8);
        return BAD_REQUEST; //形如 /uploadXYZ
    }
    // =====================================

    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        //POST 但未携带请求体时 m_string 为空，此处的解析无从进行
        if (m_string == nullptr)
            return BAD_REQUEST;

        //此处不预拼路径：注册/登录的结果随后会把 m_url 改写为跳转页面，
        //实际路径统一由下方的映射逻辑写入（见 set_real_file）

        //按 form-urlencoded 规则解析请求体，字段名与顺序都不再是解析前提
        const std::map<std::string, std::string> params = parse_form_body(m_string, m_content_length);
        auto user_it = params.find("user");
        auto passwd_it = params.find("password");
        if (user_it == params.end() || passwd_it == params.end() || user_it->second.empty())
            return BAD_REQUEST;

        const std::string &name = user_it->second;
        const std::string &password = passwd_it->second;

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            switch (register_user(name, password, mysql))
            {
            case RegisterResult::kInserted:
                strcpy(m_url, "/log.html");
                break;
            case RegisterResult::kDuplicate:
                strcpy(m_url, "/registerError.html");
                break;
            case RegisterResult::kFailed:
                LOG_ERROR("register user failed, mysql error:%s", mysql_error(mysql));
                strcpy(m_url, "/registerError.html");
                break;
            }
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            //库中保存的是加盐哈希，因此只能按哈希校验，不能直接比对原文
            std::string stored_passwd;
            if (lookup_user(name, stored_passwd) && password_hash::verify(password, stored_passwd))
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //把请求路径映射为根目录下的实际文件路径。拼接在 m_real_file 的容量内一次完成，
    //装不下即判为错误请求——此前的写法按剩余空间传长度，根目录过长时长度参数为负，
    //转为无符号后写入越界地址
    if (*(p + 1) == '0')
    {
        if (!set_real_file("/register.html"))
            return BAD_REQUEST;
    }
    else if (*(p + 1) == '1')
    {
        if (!set_real_file("/log.html"))
            return BAD_REQUEST;
    }
    else if (*(p + 1) == '5')
    {
        if (!set_real_file("/picture.html"))
            return BAD_REQUEST;
    }
    else if (*(p + 1) == '6')
    {
        if (!set_real_file("/video.html"))
            return BAD_REQUEST;
    }
    else if (*(p + 1) == '7')
    {
        if (!set_real_file("/fans.html"))
            return BAD_REQUEST;
    }
    else if (*(p + 1) == '8')
    {
        if (!set_real_file("/upload.html"))
            return BAD_REQUEST;
    }
    else
    {
        if (!set_real_file(m_url))
            return BAD_REQUEST;
    }

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //空文件不映射：长度为 0 的映射会失败，其响应由 process_write 直接给出空正文
    if (m_file_stat.st_size > 0)
    {
        const int fd = open(m_real_file, O_RDONLY);
        if (fd < 0)
            return NO_RESOURCE;

        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m_file_address == MAP_FAILED)
        {
            //映射失败时置空，避免后续 unmap 与 writev 作用于无效地址
            m_file_address = nullptr;
            return INTERNAL_ERROR;
        }
    }
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (static_cast<size_t>(bytes_have_send) >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf.data() + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
//写缓冲区按需扩容，达到上限时返回 false，由调用方转为错误处理
bool http_conn::grow_write_buffer()
{
    const size_t grown = m_write_buf.size() * 2;
    if (grown > static_cast<size_t>(MAX_RESPONSE_SIZE))
        return false;

    m_write_buf.resize(grown); //新追加的部分会被置零
    return true;
}

bool http_conn::add_response(const char *format, ...)
{
    va_list arg_list;
    va_start(arg_list, format);

    //空间不足时扩容后重试。vsnprintf 在截断时返回「本该写入的长度」，
    //因此以它是否超出可用空间来判断是否需要重试
    while (true)
    {
        const size_t available = m_write_buf.size() - static_cast<size_t>(m_write_idx);
        if (available == 0)
        {
            if (!grow_write_buffer())
            {
                va_end(arg_list);
                return false;
            }
            continue;
        }

        //每次重试都要重新取一份参数，否则已被消费的 va_list 不能再次使用
        va_list copy;
        va_copy(copy, arg_list);
        const int len = vsnprintf(m_write_buf.data() + m_write_idx, available, format, copy);
        va_end(copy);

        if (len < 0)
        {
            va_end(arg_list);
            return false;
        }
        if (static_cast<size_t>(len) >= available)
        {
            if (!grow_write_buffer())
            {
                va_end(arg_list);
                return false;
            }
            continue;
        }

        m_write_idx += len;
        break;
    }

    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf.data());

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type(const char *type)
{
    return add_response("Content-Type:%s\r\n", type);
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    if (!add_response("%s", "\r\n"))
        return false;

    //空行写完即响应头结束，正文从此处开始。HEAD 响应据此截断掉正文
    m_header_end = m_write_idx;
    return true;
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    //错误响应的正文是纯文本，统一声明类型；此前这些响应没有任何 Content-Type，
    //由客户端自行猜测
    static const char *kErrorContentType = "text/plain; charset=utf-8";

    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        //请求本身不合法，返回 400；此前返回的是 404 与「文件未找到」文案
        add_status_line(400, error_400_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
            return false;
        break;
    }
    case NO_RESOURCE:
    {
        //此前该状态没有对应分支，控制流落入 default 后连接被直接关闭，
        //客户端收不到任何响应
        add_status_line(404, error_404_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case REQUEST_TOO_LARGE:
    {
        //请求体超限。余下的字节已无从处理，因此回应后关闭连接，
        //而不沿用请求里的 keep-alive 意愿
        m_linger = false;
        add_status_line(413, error_413_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_413_form));
        if (!add_content(error_413_form))
            return false;
        break;
    }
    case REQUEST_HEADER_TOO_LARGE:
    {
        m_linger = false;
        add_status_line(431, error_431_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_431_form));
        if (!add_content(error_431_form))
            return false;
        break;
    }
    case METHOD_NOT_IMPLEMENTED:
    {
        //方法未被实现。响应中给出 Allow 告知实际支持的方法
        add_status_line(501, error_501_title);
        add_response("Allow:%s\r\n", "GET, HEAD, POST");
        add_content_type(kErrorContentType);
        add_headers(strlen(error_501_form));
        if (!add_content(error_501_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case UNSUPPORTED_MEDIA_TYPE:
    {
        add_status_line(415, error_415_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_415_form));
        if (!add_content(error_415_form))
            return false;
        break;
    }
    case EXPECTATION_FAILED:
    {
        //客户端可能没等 100 Continue 就把请求体发了过来，那些字节无从处理，
        //因此与 413 同样在回应后关闭连接
        m_linger = false;
        add_status_line(417, error_417_title);
        add_content_type(kErrorContentType);
        add_headers(strlen(error_417_form));
        if (!add_content(error_417_form))
            return false;
        break;
    }
    case DYNAMIC_CONTENT:
    {
        //正文由请求处理阶段生成（目前用于上传列表页）
        add_status_line(200, ok_200_title);
        add_content_type(m_inline_content_type.c_str());
        add_headers(static_cast<int>(m_inline_body.size()));
        if (!add_content(m_inline_body.c_str()))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);

        // ========== 新增：设置正确的 Content-Type ==========
        add_content_type(get_mime_type(m_real_file));
        // =================================================

        if (m_is_upload_download)
        {
            //上传内容来自用户，一律按附件下载：可携带脚本的类型（HTML、SVG 等）
            //若就地渲染，会构成同源的存储型 XSS
            const std::string path(m_real_file);
            const size_t slash = path.find_last_of('/');
            const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
            add_response("Content-Disposition: attachment; filename=\"%s\"\r\n", sanitize_header_value(name).c_str());
        }

        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf.data();
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;

            if (m_method == HEAD)
            {
                //HEAD 只发头部：Content-Length 仍是文件的真实长度，
                //但正文不进入发送队列
                m_iv_count = 1;
                bytes_to_send = m_write_idx;
            }
            else
            {
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
            }
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
        break;
    }

        //新增：
        // case UPLOAD_SUCCESS:
        // {
        //     // 构造风格一致的上传成功 HTML 页面
        //     std::string html_body;
        //     html_body = "<!DOCTYPE html>\r\n";
        //     html_body += "<html><head><meta charset=\"UTF-8\">";
        //     html_body += "<title>Upload Success</title></head>\r\n";
        //     html_body += "<body>\r\n<br/>\r\n<br/>\r\n";
        //     html_body += "<div align=\"center\"><font size=\"5\">";
        //     html_body += "<strong>上传成功</strong></font></div>\r\n<br/>\r\n";
        //     html_body += "<div align=\"center\"><font size=\"4\">";
        //     html_body += "文件 <strong>" + m_file_name + "</strong> 已成功上传";
        //     html_body += "</font></div>\r\n<br/>\r\n<br/>\r\n";
        //     html_body += "<div align=\"center\">\r\n";
        //     html_body += "<form action=\"8\" method=\"post\">\r\n";
        //     html_body += "<button type=\"submit\">继续上传</button>\r\n";
        //     html_body += "</form>\r\n</div>\r\n<br/>\r\n";
        //     html_body += "<div align=\"center\">\r\n";
        //     html_body += "<form action=\"5\" method=\"post\">\r\n";
        //     html_body += "<button type=\"submit\">返回主页</button>\r\n";
        //     html_body += "</form>\r\n</div>\r\n";
        //     html_body += "</body>\r\n</html>\r\n";

        //     add_status_line(200, ok_200_title);
        //     add_response("Content-Type: text/html; charset=utf-8\r\n");
        //     add_content_length(html_body.length());
        //     add_linger();
        //     add_blank_line();
        //     if (!add_content(html_body.c_str()))
        //         return false;

        //     m_iv[0].iov_base = m_write_buf;
        //     m_iv[0].iov_len = m_write_idx;
        //     m_iv_count = 1;
        //     bytes_to_send = m_write_idx;
        //     return true;
        // }

    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf.data();
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;

    //HEAD 的正文与头部同处写缓冲区，按响应头结束的位置截断
    if (m_method == HEAD && m_header_end > 0)
    {
        m_iv[0].iov_len = m_header_end;
        bytes_to_send = m_header_end;
    }
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

//新增：文件上传功能代码
void http_conn::init_file_upload_state()
{
    m_is_file_upload = false;
    m_is_upload_download = false;
    m_boundary.clear();
    m_file_name.clear();
    m_file_content.clear();
    m_inline_body.clear();
    m_inline_content_type.clear();
}

// 新增：解析 multipart/form-data 请求体，从 m_string 中提取文件名和内容
http_conn::HTTP_CODE http_conn::parse_multipart_content()
{
    if (m_content_length <= 0 || m_string == NULL)
        return BAD_REQUEST;

    std::string body(m_string, m_content_length);

    // 1. 找到起始 boundary
    size_t start = body.find(m_boundary);
    if (start == std::string::npos)
        return BAD_REQUEST;
    start += m_boundary.length();

    // 2. 跳过 \r\n
    if (start + 2 <= body.length() && body.substr(start, 2) == "\r\n")
        start += 2;
    else
        return BAD_REQUEST;

    // 3. 找到 part 头部结束位置（"\r\n\r\n"）
    size_t header_end = body.find("\r\n\r\n", start);
    if (header_end == std::string::npos)
        return BAD_REQUEST;

    // 4. 提取文件名
    std::string part_header = body.substr(start, header_end - start);
    size_t filename_pos = part_header.find("filename=\"");
    if (filename_pos == std::string::npos)
        return BAD_REQUEST;

    size_t name_start = filename_pos + 10; // 跳过 "filename=\""
    size_t name_end = part_header.find("\"", name_start);
    if (name_end == std::string::npos)
        return BAD_REQUEST;

    m_file_name = part_header.substr(name_start, name_end - name_start);

    // 安全处理：只保留文件名，丢弃路径
    size_t slash = m_file_name.find_last_of("/\\");
    if (slash != std::string::npos)
        m_file_name = m_file_name.substr(slash + 1);

    // 5. 提取文件内容
    size_t content_start = header_end + 4; // 跳过 "\r\n\r\n"
    size_t content_end = body.find(m_boundary, content_start);
    if (content_end == std::string::npos)
        return BAD_REQUEST;

    // 去掉末尾的 "\r\n"
    if (content_end >= 2 && body.substr(content_end - 2, 2) == "\r\n")
        content_end -= 2;

    //空文件是合法输入：只要求存在内容分隔，不再要求内容非空
    m_file_content = body.substr(content_start, content_end - content_start);
    return GET_REQUEST;
}

// 新增：将 m_file_content 写入上传目录
bool http_conn::save_uploaded_file()
{
    if (m_file_name.empty())
        return false;

    const std::string upload_dir(kUploadDir);
    // 确保目录存在
    if (access(upload_dir.c_str(), F_OK) != 0 && mkdir(upload_dir.c_str(), 0755) != 0)
        return false;

    //文件名为空时 open 会失败，因此上面已先判空；此处只做单段限定，
    //与读取路径的约束一致，保证写入与读取落在同一目录内
    if (m_file_name.find('/') != std::string::npos)
        return false;

    const std::string filepath = upload_dir + "/" + m_file_name;
    std::ofstream ofs(filepath.c_str(), std::ios::binary);
    if (!ofs.is_open())
        return false;

    ofs.write(m_file_content.data(), static_cast<std::streamsize>(m_file_content.size()));
    return ofs.good();
}

//返回已上传的文件。名称限定为单一段落，因此只会落在上传目录内
http_conn::HTTP_CODE http_conn::serve_uploaded_file(const char *name)
{
    const std::string filename(name);
    if (filename.empty() || filename == "." || filename == ".." || filename.find('/') != std::string::npos)
        return BAD_REQUEST;

    const std::string path = std::string(kUploadDir) + "/" + filename;
    if (stat(path.c_str(), &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!S_ISREG(m_file_stat.st_mode))
        return FORBIDDEN_REQUEST;

    //上传目录加文件名超出 m_real_file 容量时无法表示该路径，按错误请求处理，
    //否则截断后的名称会打开并非请求目标的文件
    if (path.size() >= sizeof(m_real_file))
        return BAD_REQUEST;
    snprintf(m_real_file, sizeof(m_real_file), "%s", path.c_str());

    //空文件不映射：长度为 0 的映射会失败，其响应由 process_write 直接给出空正文
    if (m_file_stat.st_size > 0)
    {
        const int fd = open(m_real_file, O_RDONLY);
        if (fd < 0)
            return NO_RESOURCE;

        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m_file_address == MAP_FAILED)
        {
            m_file_address = nullptr;
            return INTERNAL_ERROR;
        }
    }

    m_is_upload_download = true;
    return FILE_REQUEST;
}

//生成已上传文件的列表页，链接指向各文件的下载地址
http_conn::HTTP_CODE http_conn::build_upload_list()
{
    //目录不存在或不可读时按「没有文件」处理，而不是服务端错误：
    //列表页的职责是展示已有内容，此时它与「目录存在但没有内容」是同一个结果。
    //此前这里返回 500，而上传目录是运行期才按需创建的（见 save_uploaded_file），
    //服务刚启动时它确实可能还不存在
    std::vector<std::pair<std::string, long>> files;
    if (DIR *dir = opendir(kUploadDir))
    {
        while (struct dirent *entry = readdir(dir))
        {
            const std::string name(entry->d_name);
            if (name.empty() || name[0] == '.')
                continue;

            const std::string path = std::string(kUploadDir) + "/" + name;
            struct stat info;
            if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
                continue;

            files.emplace_back(name, static_cast<long>(info.st_size));
        }
        closedir(dir);
    }

    std::sort(files.begin(), files.end());

    std::string html;
    html += "<!DOCTYPE html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">";
    html += "<title>已上传文件</title></head>\n<body>\n<h2>已上传文件</h2>\n";

    if (files.empty())
    {
        html += "<p>暂无文件。</p>\n";
    }
    else
    {
        html += "<ul>\n";
        for (const auto &file : files)
        {
            //文件名来自用户输入，链接与展示都要转义
            html += "<li><a href=\"/upload/" + url_encode_segment(file.first) + "\">" + html_escape(file.first) +
                    "</a>（" + std::to_string(file.second) + " 字节）</li>\n";
        }
        html += "</ul>\n";
    }
    html += "</body></html>\n";

    m_inline_body = html;
    m_inline_content_type = "text/html; charset=utf-8";
    return DYNAMIC_CONTENT;
}