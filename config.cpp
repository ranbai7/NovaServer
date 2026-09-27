#include "config.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <unistd.h>

namespace
{
//去掉首尾空白
std::string trim(const std::string &text)
{
    const char *spaces = " \t\r\n";
    const size_t begin = text.find_first_not_of(spaces);
    if (begin == std::string::npos)
        return std::string();
    const size_t end = text.find_last_not_of(spaces);
    return text.substr(begin, end - begin + 1);
}

//去掉成对的首尾双引号，便于书写含首尾空白的取值
std::string unquote(const std::string &text)
{
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        return text.substr(1, text.size() - 2);
    return text;
}

//按十进制解析整数，要求整个字符串都是数字且不超出 int 范围
bool parse_int(const std::string &text, int &out)
{
    if (text.empty())
        return false;

    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < INT_MIN || value > INT_MAX)
        return false;

    out = static_cast<int>(value);
    return true;
}

//取出一个配置项：优先匹配「节.键」，其次匹配裸键；
//取出后从集合中删除，便于最后检查是否有无法识别的条目
void take(std::map<std::string, std::string> &items, const std::string &section, const std::string &key,
          std::string &value, bool &found)
{
    found = false;
    auto it = items.find(section + "." + key);
    if (it == items.end())
        it = items.find(key);
    if (it == items.end())
        return;

    value = it->second;
    found = true;
    items.erase(it);
}
} // namespace

Config::Config()
{
    // ---- 监听与并发 ----
    PORT = 9006;
    TRIGMode = 0; //listenfd 与 connfd 均为 LT
    OPT_LINGER = 0;
    actor_model = 0; //Proactor
    thread_num = 8;
    close_log = 0;
    root_dir = "./root";

    // ---- 数据库 ----
    db_host = "127.0.0.1";
    db_port = 3306;
    db_user = "root";
    db_password = "";
    db_name = "yourdb";
    sql_num = 8;

    // ---- 日志 ----
    LOGWrite = 0; //同步写入
    log_dir = "./ServerLog";
    log_file = "server";
    log_buf_size = 2000;
    log_split_lines = 800000;
    log_queue_size = 800;

    // ---- 配置文件 ----
    config_file = "./config.ini";
    config_file_explicit = false;
}

Config::LoadResult Config::load(const std::string &path, std::string &error)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        error = "无法打开配置文件: " + path;
        return LoadResult::kNotFound;
    }

    std::map<std::string, std::string> items;
    std::string section;
    std::string line;
    int line_no = 0;

    while (std::getline(ifs, line))
    {
        ++line_no;
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';')
            continue;

        if (text.front() == '[')
        {
            const size_t close = text.find(']');
            if (close == std::string::npos)
            {
                error = "第 " + std::to_string(line_no) + " 行: 节缺少右方括号";
                return LoadResult::kError;
            }
            section = trim(text.substr(1, close - 1));
            continue;
        }

        const size_t eq = text.find('=');
        if (eq == std::string::npos)
        {
            error = "第 " + std::to_string(line_no) + " 行: 缺少 '='";
            return LoadResult::kError;
        }

        const std::string key = trim(text.substr(0, eq));
        if (key.empty())
        {
            error = "第 " + std::to_string(line_no) + " 行: 键名为空";
            return LoadResult::kError;
        }

        items[section.empty() ? key : section + "." + key] = unquote(trim(text.substr(eq + 1)));
    }

    if (!apply(items, error))
        return LoadResult::kError;

    return LoadResult::kLoaded;
}

bool Config::apply(std::map<std::string, std::string> &items, std::string &error)
{
    std::string message;

    //逐项应用。即使某项取值非法也继续处理其余项，这样可以一次性定位文件中的全部格式问题
    auto int_item = [&](const std::string &section, const std::string &key, int &out)
    {
        std::string value;
        bool found = false;
        take(items, section, key, value, found);
        if (!found || m_cli_keys.count(key) != 0)
            return;
        if (!parse_int(value, out) && message.empty())
            message = "配置项 " + key + " 的取值不是合法整数: " + value;
    };
    auto str_item = [&](const std::string &section, const std::string &key, std::string &out)
    {
        std::string value;
        bool found = false;
        take(items, section, key, value, found);
        if (found && m_cli_keys.count(key) == 0)
            out = value;
    };

    int_item("server", "port", PORT);
    int_item("server", "trig_mode", TRIGMode);
    int_item("server", "opt_linger", OPT_LINGER);
    int_item("server", "actor_model", actor_model);
    int_item("server", "thread_num", thread_num);
    int_item("server", "close_log", close_log);
    str_item("server", "root_dir", root_dir);

    str_item("database", "host", db_host);
    int_item("database", "port", db_port);
    str_item("database", "user", db_user);
    str_item("database", "password", db_password);
    str_item("database", "database", db_name);
    int_item("database", "pool_size", sql_num);

    int_item("log", "write_mode", LOGWrite);
    str_item("log", "dir", log_dir);
    str_item("log", "file", log_file);
    int_item("log", "buf_size", log_buf_size);
    int_item("log", "split_lines", log_split_lines);
    int_item("log", "queue_size", log_queue_size);

    if (!message.empty())
    {
        error = message;
        return false;
    }

    //应用完毕后仍有剩余条目，说明存在拼写错误或已不再支持的键。
    //静默忽略会让「改了配置却没生效」难以察觉，因此直接报错
    if (!items.empty())
    {
        error = "配置文件中存在无法识别的配置项: " + items.begin()->first;
        return false;
    }

    return true;
}

int Config::int_arg(const char *text, const char *name) const
{
    int value = 0;
    if (!parse_int(text == nullptr ? std::string() : std::string(text), value))
    {
        //取值非法时终止启动，而不是像 atoi 那样静默变成 0
        std::fprintf(stderr, "参数 -%s 的取值不是合法整数: %s\n", name, text == nullptr ? "" : text);
        std::exit(EXIT_FAILURE);
    }
    return value;
}

void Config::parse_arg(int argc, char *argv[])
{
    int opt;
    const char *str = "p:l:m:o:s:t:c:a:f:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            PORT = int_arg(optarg, "p");
            m_cli_keys.insert("port");
            break;
        }
        case 'l':
        {
            LOGWrite = int_arg(optarg, "l");
            m_cli_keys.insert("write_mode");
            break;
        }
        case 'm':
        {
            TRIGMode = int_arg(optarg, "m");
            m_cli_keys.insert("trig_mode");
            break;
        }
        case 'o':
        {
            OPT_LINGER = int_arg(optarg, "o");
            m_cli_keys.insert("opt_linger");
            break;
        }
        case 's':
        {
            sql_num = int_arg(optarg, "s");
            m_cli_keys.insert("pool_size");
            break;
        }
        case 't':
        {
            thread_num = int_arg(optarg, "t");
            m_cli_keys.insert("thread_num");
            break;
        }
        case 'c':
        {
            close_log = int_arg(optarg, "c");
            m_cli_keys.insert("close_log");
            break;
        }
        case 'a':
        {
            actor_model = int_arg(optarg, "a");
            m_cli_keys.insert("actor_model");
            break;
        }
        case 'f':
        {
            config_file = optarg;
            config_file_explicit = true;
            break;
        }
        default:
            break;
        }
    }
}
