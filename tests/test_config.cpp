// 配置文件解析的单元测试
//
// 配置文件是唯一影响启动的外部输入，出错时用户只能看到一行报错。
// 因此这里既验证取值能否正确生效，也验证非法输入能否被明确拒绝
// ——「改了配置却没生效」比「启动失败」更难排查。

#include "../config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

namespace
{
class ConfigTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/novaserver_config_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        m_dir = dir;
        m_path = m_dir + "/config.ini";
    }

    void TearDown() override
    {
        std::remove(m_path.c_str());
        rmdir(m_dir.c_str());
    }

    Config::LoadResult load(const std::string &content, Config &config, std::string &error) const
    {
        std::ofstream ofs(m_path);
        ofs << content;
        ofs.close();
        return config.load(m_path, error);
    }

    void load_ok(const std::string &content, Config &config) const
    {
        std::string error;
        ASSERT_EQ(load(content, config, error), Config::LoadResult::kLoaded) << error;
    }

    void load_fail(const std::string &content, const std::string &keyword) const
    {
        Config config;
        std::string error;
        ASSERT_EQ(load(content, config, error), Config::LoadResult::kError);
        EXPECT_NE(error.find(keyword), std::string::npos) << "实际报错: " << error;
    }

    std::string m_dir;
    std::string m_path;
};

TEST_F(ConfigTest, MissingFileIsReportedAsNotFound)
{
    Config config;
    std::string error;
    EXPECT_EQ(config.load(m_dir + "/does-not-exist.ini", error), Config::LoadResult::kNotFound);
}

TEST_F(ConfigTest, AppliesSectionedValues)
{
    Config config;
    load_ok(R"(
[server]
port = 9100
trig_mode = 3
root_dir = /srv/www

[database]
host = db.internal
port = 3307
user = nova
password = "p w"
database = novadb
pool_size = 16

[log]
write_mode = 1
dir = ./logs
file = app
buf_size = 4096
split_lines = 100
queue_size = 64
)",
            config);

    EXPECT_EQ(config.PORT, 9100);
    EXPECT_EQ(config.TRIGMode, 3);
    EXPECT_EQ(config.root_dir, "/srv/www");
    EXPECT_EQ(config.db_host, "db.internal");
    EXPECT_EQ(config.db_port, 3307);
    EXPECT_EQ(config.db_user, "nova");
    EXPECT_EQ(config.db_password, "p w"); //双引号保留首尾空白
    EXPECT_EQ(config.db_name, "novadb");
    EXPECT_EQ(config.sql_num, 16);
    EXPECT_EQ(config.LOGWrite, 1);
    EXPECT_EQ(config.log_dir, "./logs");
    EXPECT_EQ(config.log_file, "app");
    EXPECT_EQ(config.log_buf_size, 4096);
    EXPECT_EQ(config.log_split_lines, 100);
    EXPECT_EQ(config.log_queue_size, 64);
}

TEST_F(ConfigTest, KeepsDefaultsForAbsentKeys)
{
    Config config;
    load_ok("[server]\nport = 9101\n", config);
    EXPECT_EQ(config.PORT, 9101);
    EXPECT_EQ(config.thread_num, 8); //文件中未给出的项沿用默认值
    EXPECT_EQ(config.db_port, 3306);
    EXPECT_EQ(config.log_file, "server");
}

TEST_F(ConfigTest, AcceptsBareKeysAndComments)
{
    Config config;
    load_ok(R"(
# 这是注释
; 这也是注释

port = 9102

# 行内的空白会被去掉
    trig_mode    =    2
)",
            config);

    EXPECT_EQ(config.PORT, 9102);
    EXPECT_EQ(config.TRIGMode, 2);
}

TEST_F(ConfigTest, RejectsUnknownKey)
{
    //拼写错误必须报错：「改了配置却没生效」比启动失败更难排查
    load_fail("[server]\nprot = 9006\n", "prot");
}

TEST_F(ConfigTest, RejectsNonIntegerValue)
{
    load_fail("[server]\nport = 9a06\n", "port");
    load_fail("[database]\npool_size = -1x\n", "pool_size");
}

TEST_F(ConfigTest, RejectsMalformedLines)
{
    load_fail("[server]\nport 9006\n", "第 2 行");
    load_fail("[server\nport = 9006\n", "右方括号");
    load_fail("[server]\n = 9006\n", "键名为空");
}

TEST_F(ConfigTest, CommandLineValueWinsOverFile)
{
    Config config;
    char arg0[] = "server";
    char arg1[] = "-p";
    char arg2[] = "9999";
    char *argv[] = {arg0, arg1, arg2, nullptr};

    optind = 1; //重置 getopt 的解析位置
    config.parse_arg(3, argv);
    ASSERT_EQ(config.PORT, 9999);

    load_ok("[server]\nport = 9006\nthread_num = 4\n", config);
    EXPECT_EQ(config.PORT, 9999);    //命令行已显式给出，配置文件不得覆盖
    EXPECT_EQ(config.thread_num, 4); //命令行未给出的项仍由配置文件生效
}

TEST_F(ConfigTest, InvalidCommandLineIntegerAbortsStartup)
{
    Config config;
    char arg0[] = "server";
    char arg1[] = "-p";
    char arg2[] = "abc";
    char *argv[] = {arg0, arg1, arg2, nullptr};

    //不能像 atoi 那样静默变成 0
    optind = 1;
    EXPECT_EXIT(config.parse_arg(3, argv), ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

TEST_F(ConfigTest, ExplicitConfigPathIsRecorded)
{
    Config config;
    char arg0[] = "server";
    char arg1[] = "-f";
    char arg2[] = "/etc/novaserver.ini";
    char *argv[] = {arg0, arg1, arg2, nullptr};

    EXPECT_FALSE(config.config_file_explicit);
    optind = 1;
    config.parse_arg(3, argv);
    EXPECT_TRUE(config.config_file_explicit);
    EXPECT_EQ(config.config_file, "/etc/novaserver.ini");
}
} // namespace
