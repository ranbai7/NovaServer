// URL 转义解码与路径规范化的单元测试
//
// 路径规范化是「请求路径是否落在 Web 根目录之内」这一安全边界的判定逻辑，
// 单靠端到端测试难以覆盖「编码后再穿越」这类输入组合，因此直接对纯函数逐一验证。

#include "../http/url_codec.h"

#include <gtest/gtest.h>
#include <string>

namespace
{
// ---------------- decode ----------------

TEST(UrlDecode, LeavesPlainTextUnchanged)
{
    EXPECT_EQ(url_codec::decode("judge.html", false), "judge.html");
    EXPECT_EQ(url_codec::decode("", false), "");
}

TEST(UrlDecode, DecodesHexEscapes)
{
    EXPECT_EQ(url_codec::decode("%20", false), " ");
    EXPECT_EQ(url_codec::decode("a%2Fb", false), "a/b");
    EXPECT_EQ(url_codec::decode("%2f", false), "/"); // 小写十六进制同样识别
    EXPECT_EQ(url_codec::decode("%25", false), "%"); // %25 解码为 % 本身
    EXPECT_EQ(url_codec::decode("%2e%2e%2f", false), "../");
}

TEST(UrlDecode, KeepsMalformedEscapesLiteral)
{
    EXPECT_EQ(url_codec::decode("%ZZ", false), "%ZZ");
    EXPECT_EQ(url_codec::decode("%A", false), "%A"); // 结尾不完整
    EXPECT_EQ(url_codec::decode("%2", false), "%2"); // 结尾不完整
    EXPECT_EQ(url_codec::decode("100%", false), "100%");
}

TEST(UrlDecode, PlusBecomesSpaceOnlyWhenRequested)
{
    // 表单请求体与查询串：'+' 表示空格
    EXPECT_EQ(url_codec::decode("a+b", true), "a b");
    EXPECT_EQ(url_codec::decode("a%2Bb", true), "a+b"); // 编码的 '+' 仍是字面量
    // 路径：'+' 是普通字符，只有 %20 才是空格
    EXPECT_EQ(url_codec::decode("a+b", false), "a+b");
    EXPECT_EQ(url_codec::decode("a%20b", false), "a b");
}

// ---------------- normalize_path ----------------

TEST(NormalizePath, KeepsCanonicalPaths)
{
    EXPECT_EQ(url_codec::normalize_path("/"), "/");
    EXPECT_EQ(url_codec::normalize_path(""), "/");
    EXPECT_EQ(url_codec::normalize_path("/judge.html"), "/judge.html");
    EXPECT_EQ(url_codec::normalize_path("/a/b/c.txt"), "/a/b/c.txt");
}

TEST(NormalizePath, CollapsesEmptyAndCurrentSegments)
{
    EXPECT_EQ(url_codec::normalize_path("/./judge.html"), "/judge.html");
    EXPECT_EQ(url_codec::normalize_path("//judge.html"), "/judge.html");
    EXPECT_EQ(url_codec::normalize_path("/a//b"), "/a/b");
    EXPECT_EQ(url_codec::normalize_path("/a/./b/"), "/a/b");
}

TEST(NormalizePath, ResolvesParentSegmentsWithinRoot)
{
    EXPECT_EQ(url_codec::normalize_path("/a/b/../c"), "/a/c");
    EXPECT_EQ(url_codec::normalize_path("/a/b/.."), "/a");
    EXPECT_EQ(url_codec::normalize_path("/a/.."), "/");
    EXPECT_EQ(url_codec::normalize_path("/a/b/../../c"), "/c");
}

TEST(NormalizePath, RejectsEscapesAboveRoot)
{
    // 回退层数超过已有一层级时，返回空串表示越界
    EXPECT_EQ(url_codec::normalize_path("/.."), "");
    EXPECT_EQ(url_codec::normalize_path("/../etc/passwd"), "");
    EXPECT_EQ(url_codec::normalize_path("/a/../../b"), "");
    // 先退到根、再退一级同样算越界，不能因为中途回到过根就放行
    EXPECT_EQ(url_codec::normalize_path("/a/b/../../../a"), "");
    EXPECT_EQ(url_codec::normalize_path(".."), "");
    EXPECT_EQ(url_codec::normalize_path("../x"), "");
}

TEST(NormalizePath, TreatsDotsInsideSegmentAsLiteral)
{
    // "...." 不是 ".."，是一个普通的名字
    EXPECT_EQ(url_codec::normalize_path("/....//x"), "/..../x");
    EXPECT_EQ(url_codec::normalize_path("/..hidden"), "/..hidden");
    EXPECT_EQ(url_codec::normalize_path("/a..b"), "/a..b");
}

TEST(NormalizePath, EscapeViaEncodingIsCaughtOnlyAfterDecoding)
{
    // 解码必须发生在规范化之前，否则编码形式可以绕过越界判断
    const std::string encoded = "/%2e%2e/%2e%2e/etc/passwd";
    EXPECT_EQ(url_codec::normalize_path(url_codec::decode(encoded, false)), "");

    // 若顺序颠倒（先规范化后解码），越界路径会被当作普通名字放行——此处固化该反例
    EXPECT_EQ(url_codec::decode(url_codec::normalize_path(encoded), false), "/../../etc/passwd");
}
} // namespace
