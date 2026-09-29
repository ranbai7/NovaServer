#include "config.h"
#include "net/signal_watcher.h"
#include "webserver.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char *argv[])
{
    //退出信号必须在创建**任何**线程之前屏蔽：信号掩码是线程属性，子线程继承的是
    //创建它那一刻的掩码。日志的写盘线程（异步写入，默认开启）在 WebServer 的
    //log_write() 里就建好了，早于 run()，因此这一步只能放在这里——留在 run() 里
    //会让写盘线程带着未屏蔽的掩码，SIGTERM 被投递给它时按默认动作终止进程，
    //表现是退出码 143 而不是优雅退出的 0
    if (!SignalWatcher::block_signals({SIGTERM, SIGINT}))
        std::fprintf(stderr, "屏蔽退出信号失败: %s\n", strerror(errno));

    //命令行解析：先取到 -f 指定的配置文件路径，并记录显式给出的键
    Config config;
    config.parse_arg(argc, argv);

    //配置文件取值，再与命令行取值合并（命令行优先）
    std::string error;
    switch (config.load(config.config_file, error))
    {
    case Config::LoadResult::kLoaded:
        break;
    case Config::LoadResult::kNotFound:
        //-f 显式指定却找不到文件，多半是路径写错，不应静默回退
        if (config.config_file_explicit)
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return EXIT_FAILURE;
        }
        std::fprintf(stderr, "未找到 %s，使用内置默认值（数据库口令等需经配置文件或命令行提供）\n",
                     config.config_file.c_str());
        break;
    case Config::LoadResult::kError:
        std::fprintf(stderr, "%s\n", error.c_str());
        return EXIT_FAILURE;
    }

    WebServer server;

    //初始化：解析配置与站点根目录
    server.init(config);

    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //建立监听并进入事件循环，直到收到退出信号
    server.run();

    return 0;
}
