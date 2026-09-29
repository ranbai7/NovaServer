// 日志子系统测试
//
// 关注两件事：
//
// 1. **缓冲区的边界**。write_log 先写时间前缀、再以 vsnprintf 追加格式化内容，
//    最后补换行与终止符；任一步的长度夹取写错都会越界。这类越界不会改变可观测的
//    输出——越界写下的内容随后就被覆盖或落在终止符之后——因此**只有在
//    AddressSanitizer 下运行才能发现**，单靠断言看不出来。
//    本文件的截断用例因此需要以 -DENABLE_ASAN=ON 构建后执行：
//        cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
//        cmake --build build-asan && ctest --test-dir build-asan
//
// 2. **行不丢、序不乱**。批量落盘把「一行一个写调用」变成「一块一个写调用」，
//    代价是多出了一层缓冲与一个写盘线程。这层结构一旦出错，表现是偶发的丢行、
//    重复或错序，ASan 与 TSan 都发现不了，只能靠下面的计数与顺序断言兜住。

#include "../log/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
//轮转文件名的后缀：主文件为 0，.1、.2 … 依次递增
int suffix_of(const std::string &name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return 0;
    return std::atoi(name.c_str() + dot + 1);
}

class LogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/novaserver_log_test_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        m_dir = dir;
        init_log();
    }

    void TearDown() override
    {
        //先把内存里的内容落盘，再删目录：否则写盘线程可能落到已删除的文件上
        Log::get_instance()->flush();
        for (const std::string &f : entries())
            std::remove((m_dir + "/" + f).c_str());
        rmdir(m_dir.c_str());
    }

    //init 可重复调用：它会先停掉上一个写盘线程、排空并关闭上一个文件
    void init_log(int buf_size = 2000, int batch_size = 65536, int flush_ms = 1000, int write_mode = 1,
                  int split_lines = 800000)
    {
        LogConfig config;
        config.dir = m_dir;
        config.file = "unit";
        config.close_log = 0;
        config.write_mode = write_mode;
        config.log_buf_size = buf_size;
        config.split_lines = split_lines;
        config.batch_buf_size = batch_size;
        config.flush_interval_ms = flush_ms;
        ASSERT_TRUE(Log::get_instance()->init(config));
    }

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

    //按轮转顺序排列的文件名
    std::vector<std::string> ordered_files() const
    {
        std::vector<std::string> names = entries();
        std::sort(names.begin(), names.end(),
                  [](const std::string &a, const std::string &b) { return suffix_of(a) < suffix_of(b); });
        return names;
    }

    //按轮转顺序读取所有日志行（不含换行符）
    std::vector<std::string> read_lines() const
    {
        std::vector<std::string> lines;
        for (const std::string &f : ordered_files())
        {
            std::ifstream ifs(m_dir + "/" + f);
            std::string line;
            while (std::getline(ifs, line))
            {
                if (!line.empty())
                    lines.push_back(line);
            }
        }
        return lines;
    }

    long long file_bytes() const
    {
        long long total = 0;
        for (const std::string &f : entries())
        {
            struct stat st = {};
            if (stat((m_dir + "/" + f).c_str(), &st) == 0)
                total += st.st_size;
        }
        return total;
    }

    //文件里「普通日志行」与「被丢弃的行数」的统计。
    //
    //写盘速度跟不上时实现会丢弃新行、并把累计丢弃数作为一行写进日志，因此核对
    //行数时必须把这两者分开：只看普通行会把按设计的丢弃误判成丢行，而只看总数
    //又会把真正的丢行掩盖掉
    struct Tally
    {
        long long normal = 0;
        long long dropped = 0;
    };

    Tally tally() const
    {
        Tally t;
        for (const std::string &f : entries())
        {
            std::ifstream ifs(m_dir + "/" + f);
            std::string line;
            while (std::getline(ifs, line))
            {
                //丢弃记录形如「… [warn]: 已丢弃 N 行日志（…）」，其余都是普通日志行
                int n = 0;
                if (std::sscanf(line.c_str(), "%*s %*s %*s 已丢弃 %d 行日志", &n) == 1)
                    t.dropped += n;
                else if (!line.empty())
                    ++t.normal;
            }
        }
        return t;
    }

    std::string m_dir;
};

//单行长度上限：无论 cap 取何值，写回的行都必须落在 cap 之内
//
//参数里带上小于 48 的 cap：既有实现用 snprintf(m_buf, 48, ...) 写时间前缀，
//而缓冲区只有 buf_size 字节，buf_size < 48 时就是一次堆越界写。这里把它钉住
TEST_F(LogTest, LineNeverExceedsBufferSize)
{
    size_t seen = 0;
    for (int cap : {128, 200, 256, 2000})
    {
        //init 会重新打开同一个文件（追加模式），因此每轮只看本轮新增的行
        init_log(cap);
        //消息长度围绕 cap 取样：刚好装得下、刚好超出、以及超出许多
        for (size_t len : {size_t(1), size_t(cap) - 1, size_t(cap), size_t(cap) + 1, size_t(cap) * 8})
        {
            const std::string msg(len, 'A');
            Log::get_instance()->write_log(1, "%s", msg.c_str());
        }
        Log::get_instance()->flush();

        const std::vector<std::string> lines = read_lines();
        ASSERT_EQ(lines.size(), seen + 5) << "cap=" << cap;
        for (size_t i = seen; i < lines.size(); ++i)
            EXPECT_LE(lines[i].size(), static_cast<size_t>(cap)) << "cap=" << cap;
        seen = lines.size();
    }
}

//配置里的单行上限过小时，init 把它夹到下限
//
//既有实现按配置值分配缓冲区，却用固定的 48 字节写时间前缀，因此 buf_size < 48
//就是一次堆越界写——用 ASan 可稳定复现。夹取之后这条路径不再可能越界，
//所以本用例的判据是「在 ASan 下不报 heap-buffer-overflow」，断言只是顺带确认
//消息确实被按上限裁剪了
TEST_F(LogTest, OutOfRangeBufferSizeIsClamped)
{
    init_log(16); //远低于下限
    const std::string huge(4096, 'B');
    Log::get_instance()->write_log(1, "%s", huge.c_str());
    Log::get_instance()->flush();

    const std::vector<std::string> lines = read_lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_LE(lines[0].size(), static_cast<size_t>(Log::MIN_LINE_BUF));
}

//短消息不应被裁剪，内容需完整保留
TEST_F(LogTest, ShortMessageIsWrittenIntact)
{
    Log::get_instance()->write_log(1, "hello %s", "world");
    Log::get_instance()->flush();

    const std::vector<std::string> lines = read_lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("hello world"), std::string::npos);
}

//flush 返回后，此前写入的行必须已经全部可见。
//
//写盘线程在锁外完成文件写入，而 flush 靠「块序号」判断排空是否完成：
//若目标序号取错（例如取排空结束时的值而不是取走待写队列那一刻的值），
//flush 就会提前返回，日志还留在内存里。这类缺陷 ASan 与 TSan 都发现不了
TEST_F(LogTest, FlushMakesEverythingDurable)
{
    const int kLines = 5000;
    //块取 1 MiB：池中共 9 块，远大于用例写入的总量，因此不会触发背压丢弃，
    //行数断言才是确定的
    init_log(2000, 1 << 20);
    for (int i = 0; i < kLines; ++i)
        Log::get_instance()->write_log(1, "line-%d", i);
    Log::get_instance()->flush();

    ASSERT_EQ(read_lines().size(), static_cast<size_t>(kLines));
}

//持续写入的同时反复 flush：flush 是屏障，且一行都不能凭空消失
//
//与 FlushMakesEverythingDurable 的区别在于这里**边写边排空**——生产线程持续交出
//缓冲块，写盘线程同时在排空，两者交错。前面那条用例是「写完再 flush」，走不到
//这个交错上去。块取小（4096）是为了让块的回转足够频繁，交错的机会更多。
//
//块小则写盘线程可能跟不上，于是**背压丢弃会真的发生**。这里不去消除它（消除它
//就得把块调大，交错也就没了），而是把丢弃计数一并核对：守恒式
//「普通行 + 丢弃 = 尝试写入」在有丢弃时同样成立，是比「行数相等」更准的判据。
//若 flush 提前返回，缺失的行既不在普通行里、也不在丢弃计数里，守恒式就会破
TEST_F(LogTest, NoLineLostWhileProducingAndFlushing)
{
    init_log(2000, 4096, 3600 * 1000);

    //给生产量设上限：下面的每轮核对都要通读一遍文件，而文件是一直在长的。
    //不设上限的话，越往后每轮越慢，在 Sanitizer 下会拖到实际不可收敛
    const long long kMaxLines = 20000;

    std::atomic<bool> stop{false};
    std::atomic<long long> written{-1}; //已完成写入的最大行号
    std::thread producer(
        [&]
        {
            long long k = 0;
            while (!stop.load(std::memory_order_relaxed) && k < kMaxLines)
            {
                Log::get_instance()->write_log(1, "p-%lld", k);
                written.store(k, std::memory_order_release);
                ++k;
            }
        });

    for (int round = 0; round < 20; ++round)
    {
        const long long upto = written.load(std::memory_order_acquire);
        Log::get_instance()->flush();
        //flush 返回时，调用之前写入的行要么已落盘，要么已计入丢弃数。
        //这里用 >= 而不是 ==：统计期间文件还在增长，可能读到写了一半的行，
        //那种情形只会让计数偏多
        const Tally t = tally();
        ASSERT_GE(t.normal + t.dropped, upto + 1)
            << "第 " << round << " 轮：flush 已返回，但调用前写入的行既没落盘也没计入丢弃";
    }

    stop.store(true);
    producer.join();
    Log::get_instance()->flush();

    //生产已停止，文件不再变化，守恒式可以按等号核对
    const Tally t = tally();
    ASSERT_EQ(t.normal + t.dropped, written.load() + 1)
        << "普通行 " << t.normal << " + 丢弃 " << t.dropped << " != 写入 " << written.load() + 1;
}

//异步模式下无需调用 flush：写盘线程会按刷新间隔自行把内容落盘
//
//这条覆盖的是「定时醒来的意义」——块没写满时也要按间隔落盘。若写盘线程只排空
//写满的块、不管 front 里未满的那一块，日志就会一直留在内存里，直到恰好写满一块
//或进程退出；在这个缺陷下文件始终为空。刷新间隔取小，等待若干个间隔后应当可见
TEST_F(LogTest, AsyncFlushesOnIntervalWithoutExplicitFlush)
{
    init_log(2000, 65536, /*flush_ms=*/50);
    Log::get_instance()->write_log(1, "periodic flush");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (file_bytes() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::vector<std::string> lines = read_lines();
    ASSERT_EQ(lines.size(), 1u) << "定时刷新未把未写满的块落盘";
    EXPECT_NE(lines[0].find("periodic flush"), std::string::npos);
}

//异步模式下，没有调用 flush 时也不应逐行落盘
//
//刷新间隔设得远大于用例时长，且只写一行（远小于缓冲块），因此写盘线程仍在等待，
//文件应当还是空的。这条正是本次改造的核心：写入不再逐行触发写文件
TEST_F(LogTest, AsyncDoesNotWriteBeforeFlush)
{
    init_log(2000, 65536, /*flush_ms=*/10000);
    Log::get_instance()->write_log(1, "one line");
    EXPECT_EQ(file_bytes(), 0);

    Log::get_instance()->flush();
    EXPECT_GT(file_bytes(), 0);
}

//同步模式保持「写入即落盘」：不调用 flush 也应立即可见
TEST_F(LogTest, SyncWritesImmediately)
{
    init_log(2000, 65536, 1000, /*write_mode=*/0);
    Log::get_instance()->write_log(1, "immediate");

    const std::vector<std::string> lines = read_lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("immediate"), std::string::npos);
}

//多线程并发写入：行数不丢不重，每行完整无撕裂，且同一线程自己的顺序严格递增
//
//行带标签是为了区分「丢行」与「行被截断成两半」——后者会让标签无法匹配
TEST_F(LogTest, ConcurrentLinesAreIntactAndOrdered)
{
    const int kThreads = 4;
    const int kPerThread = 2000;
    //同样把块取到 1 MiB：避免背压丢弃混进来，行数断言才是确定的
    init_log(2000, 1 << 20);
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t)
    {
        writers.emplace_back(
            [t, kPerThread]
            {
                for (int i = 0; i < kPerThread; ++i)
                    Log::get_instance()->write_log(1, "thr=%d seq=%d", t, i);
            });
    }
    for (std::thread &w : writers)
        w.join();
    Log::get_instance()->flush();

    const std::vector<std::string> lines = read_lines();
    ASSERT_EQ(lines.size(), static_cast<size_t>(kThreads * kPerThread));

    std::vector<int> last_seen(kThreads, -1);
    for (const std::string &line : lines)
    {
        //从标签处解析，不对前缀的字段数作假设
        const char *tag = strstr(line.c_str(), "thr=");
        ASSERT_NE(tag, nullptr) << "行没有完整写出: " << line;
        int t = -1;
        int seq = -1;
        ASSERT_EQ(std::sscanf(tag, "thr=%d seq=%d", &t, &seq), 2)
            << "行被截断或写入时与相邻内容交错: " << line;
        ASSERT_GE(t, 0);
        ASSERT_LT(t, kThreads);
        EXPECT_GT(seq, last_seen[t]) << "线程 " << t << " 的行序出现倒退或重复: " << line;
        last_seen[t] = seq;
    }
    for (int t = 0; t < kThreads; ++t)
        EXPECT_EQ(last_seen[t], kPerThread - 1);
}

//反复 init 不应泄漏文件句柄
//
//既有实现直接重新 fopen，上一份句柄就此泄漏；而 init 在单元测试里会被反复调用，
//因此这个缺陷在测试中必然暴露
TEST_F(LogTest, RepeatedInitDoesNotLeakFd)
{
    const auto count_fds = []()
    {
        int n = 0;
        DIR *dir = opendir("/proc/self/fd");
        EXPECT_NE(dir, nullptr);
        while (struct dirent *ent = readdir(dir))
        {
            if (ent->d_name[0] != '.')
                ++n;
        }
        closedir(dir);
        return n;
    };

    init_log();
    const int before = count_fds();
    for (int i = 0; i < 20; ++i)
    {
        init_log();
        Log::get_instance()->write_log(1, "round %d", i);
        Log::get_instance()->flush();
    }
    EXPECT_EQ(count_fds(), before);
}

//轮转：把各后缀文件按顺序拼接，得到的行序列必须与写入序列完全一致
//
//这条断言一次覆盖「轮转时不丢行、不重行、不乱序」三件事——它们都逃得过
//ASan 与 TSan 的检查，只能用内容对齐来确认。
//
//轮转按缓冲块判定，因此要让 split_lines 小于一块的行数，才真的会切出多个文件。
//块取 8192（约 170 行一块）而 split_lines 取 100，即属于此。
//
//块又不取更小，是为了让池（9 块共 72 KiB）容得下本用例的全部输出（约 48 KiB），
//从而不会触发背压丢弃——否则行序列里会混进丢弃记录，顺序断言就无从谈起了
TEST_F(LogTest, RotationPreservesOrder)
{
    const int kLines = 1000;
    init_log(2000, 8192, 1000, /*write_mode=*/1, /*split_lines=*/100);
    for (int i = 0; i < kLines; ++i)
        Log::get_instance()->write_log(1, "line-%d", i);
    Log::get_instance()->flush();

    //行数超过 split_lines，应当发生过轮转
    EXPECT_GE(ordered_files().size(), 2u);

    const std::vector<std::string> lines = read_lines();
    ASSERT_EQ(lines.size(), static_cast<size_t>(kLines));
    for (int i = 0; i < kLines; ++i)
    {
        const std::string expected = "line-" + std::to_string(i);
        ASSERT_NE(lines[i].find(expected), std::string::npos) << "第 " << i << " 行: " << lines[i];
    }
}

//背压记账：文件中的普通行数加上丢弃计数，必须等于尝试写入的总行数
//
//写盘速度跟不上时实现会丢弃新行并计数，再由写盘线程把计数写进日志，
//使丢失在文件里可见而不是无声无息。这里验证「守恒」这条不变式。
//
//参数是为了让丢弃**必然发生**而选的：单行取到长度上限（4096），缓冲块也取
//最小值（4096），于是一个块只装得下一行，池中 9 块不到十条日志就被填满。
//此时生产速度远超写盘速度，丢弃几乎必然出现——不做这个设置的话，
//写盘线程总能在池满之前排空，丢弃分支就永远走不到
TEST_F(LogTest, BackpressureAccountIsBalanced)
{
    const int kThreads = 4;
    const int kPerThread = 500;
    const size_t kTotal = static_cast<size_t>(kThreads) * kPerThread;

    init_log(Log::MAX_LINE_BUF, Log::MAX_LINE_BUF, 3600 * 1000);

    const std::string payload(Log::MAX_LINE_BUF, 'x');
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t)
    {
        writers.emplace_back(
            [t, kPerThread, &payload]
            {
                for (int i = 0; i < kPerThread; ++i)
                    Log::get_instance()->write_log(1, "thr=%d seq=%d %s", t, i, payload.c_str());
            });
    }
    for (std::thread &w : writers)
        w.join();
    Log::get_instance()->flush();

    const Tally t = tally();
    EXPECT_EQ(static_cast<size_t>(t.normal + t.dropped), kTotal)
        << "普通行 " << t.normal << " + 丢弃 " << t.dropped << " != 写入 " << kTotal;

    //上面的守恒式在「一次丢弃都没发生」时也成立，因此再加一条：确认丢弃路径
    //真的被走到了，而不是被静默跳过
    EXPECT_GT(t.dropped, 0) << "未触发背压，丢弃路径未被覆盖";
}
} // namespace
