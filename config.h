#ifndef CONFIG_H
#define CONFIG_H

#include <map>
#include <set>
#include <string>

//运行配置。
//
//取值来源分三层，优先级由低到高：
//  1. 构造函数中的内置默认值
//  2. 配置文件（load）
//  3. 命令行参数（parse_arg）
//
//配置文件只覆盖它显式给出的键；命令行显式给出的键不会被配置文件改写，
//因此二者可以混用，例如用配置文件存放数据库口令，用命令行临时改端口。
class Config
{
public:
    enum class LoadResult
    {
        kLoaded,   //成功读取并应用
        kNotFound, //文件不存在，可继续使用默认值与命令行取值
        kError     //文件存在但内容非法，error 给出原因
    };

    Config();

    //从配置文件加载。配置文件按 [节] 分组，节内形如 键 = 值 ；
    //以 # 或 ; 开头的行为注释。取值可加双引号以保留首尾空白
    LoadResult load(const std::string &path, std::string &error);

    //解析命令行参数，并记录显式给出的键
    void parse_arg(int argc, char *argv[]);

    // ---- 监听与并发 ----
    int PORT;
    int TRIGMode;
    int OPT_LINGER;
    int thread_num;
    int close_log;
    std::string root_dir;

    // ---- 数据库 ----
    std::string db_host;
    int db_port;
    std::string db_user;
    std::string db_password;
    std::string db_name;
    int sql_num;

    // ---- 日志 ----
    int LOGWrite;
    std::string log_dir;
    std::string log_file;
    int log_buf_size;
    int log_split_lines;
    int log_queue_size;

    // ---- 配置文件本身 ----
    std::string config_file;   //-f 指定的路径
    bool config_file_explicit; //-f 是否显式给出：显式给出时文件缺失视为错误

private:
    //把配置项应用到对应成员。取值非法时记录原因并返回 false
    bool apply(std::map<std::string, std::string> &items, std::string &error);
    //按下标提取命令行整数参数，非法取值直接终止启动
    int int_arg(const char *text, const char *name) const;

    //命令行显式给出的键名，配置文件不得覆盖
    std::set<std::string> m_cli_keys;
};

#endif
