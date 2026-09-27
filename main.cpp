#include "config.h"
#include "webserver.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char *argv[])
{
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
