// 日志写入边界测试
//
// write_log 先写入时间前缀，再以 vsnprintf 追加格式化内容，最后补换行与终止符。
// 当格式化内容长于缓冲区时 vsnprintf 会截断，但它返回的是「本该写入的长度」而非
// 实际写入长度；若实现直接据此定位结尾，换行与终止符就会落到缓冲区之外。
//
// 值得注意的是：这个越界不会改变任何可观测的输出。vsnprintf 在截断处就已经写好
// 终止符，多出来的换行与终止符落在终止符之后，日志字符串本身始终是短的，因此单纯
// 断言输出长度无法区分修复前后——真正的判据是在 **AddressSanitizer 下运行时是否
// 报告 heap-buffer-overflow**。
//
// 本用例因此需要以 -DENABLE_ASAN=ON 构建后执行：
//     cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
//     cmake --build build-asan && ctest --test-dir build-asan
// 下面的断言只是防止回归到「消息被过度裁剪」的另一端，不承担发现越界的职责。

#include "../log/log.h"

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
const int kBufSize = 256;

class LogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/novaserver_log_test_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        m_dir = dir;
        // 注意：init 会在文件名前追加日期前缀，因此实际文件名与入参并不相同，
        // 这里传入的只是一段用于识别的后缀
        const std::string name = m_dir + "/unit.log";
        ASSERT_TRUE(Log::get_instance()->init(name.c_str(), 0, kBufSize, 10000, 0));
    }

    void TearDown() override
    {
        for (const std::string &f : entries())
            std::remove((m_dir + "/" + f).c_str());
        rmdir(m_dir.c_str());
    }

    // 列出临时目录下的普通文件
    std::vector<std::string> entries() const
    {
        std::vector<std::string> names;
        DIR *dir = opendir(m_dir.c_str());
        if (dir == nullptr)
            return names;
        while (struct dirent *ent = readdir(dir))
        {
            const std::string name = ent->d_name;
            if (name == "." || name == "..")
                continue;
            names.push_back(name);
        }
        closedir(dir);
        return names;
    }

    // 读取日志文件中最后一行（不含换行符）
    std::string last_line() const
    {
        const std::vector<std::string> files = entries();
        if (files.empty())
            return std::string();

        std::ifstream ifs(m_dir + "/" + files.front());
        std::string line;
        std::string last;
        while (std::getline(ifs, line))
        {
            if (!line.empty())
                last = line;
        }
        return last;
    }

    std::string m_dir;
};

// 超长消息触发截断，写回的行必须落在缓冲区长度之内
TEST_F(LogTest, TruncatedMessageStaysWithinBuffer)
{
    const std::string huge(kBufSize * 8, 'A');
    Log::get_instance()->write_log(1, "%s", huge.c_str());
    Log::get_instance()->flush();

    const std::string line = last_line();
    ASSERT_FALSE(line.empty());
    EXPECT_LE(line.size(), static_cast<size_t>(kBufSize));
}

// 短消息不应被裁剪，内容需完整保留
TEST_F(LogTest, ShortMessageIsWrittenIntact)
{
    Log::get_instance()->write_log(1, "hello %s", "world");
    Log::get_instance()->flush();

    const std::string line = last_line();
    EXPECT_NE(line.find("hello world"), std::string::npos);
}
} // namespace
