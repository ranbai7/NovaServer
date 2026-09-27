// 口令加盐哈希的单元测试
//
// 口令校验的正确性无法从端到端行为上确认——「用错误的实现也可能让正确口令通过」，
// 因此这里直接针对编码格式、盐的随机性、存储值被篡改后的行为逐项验证。

#include "../auth/password_hash.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
//把编码串中的迭代次数字段替换为给定取值
std::string with_iterations(const std::string &encoded, const std::string &iterations)
{
    const size_t first = encoded.find('$');
    const size_t second = encoded.find('$', first + 1);
    return encoded.substr(0, first + 1) + iterations + encoded.substr(second);
}

//按 '$' 切分
std::vector<std::string> split(const std::string &text)
{
    std::vector<std::string> parts;
    size_t begin = 0;
    while (true)
    {
        const size_t pos = text.find('$', begin);
        if (pos == std::string::npos)
        {
            parts.push_back(text.substr(begin));
            break;
        }
        parts.push_back(text.substr(begin, pos - begin));
        begin = pos + 1;
    }
    return parts;
}

TEST(PasswordHash, EncodeProducesIdentifiableFormat)
{
    const std::string encoded = password_hash::encode("correct horse");
    ASSERT_FALSE(encoded.empty());

    const std::vector<std::string> parts = split(encoded);
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0], "pbkdf2_sha256");
    EXPECT_EQ(parts[1], "100000");
    EXPECT_FALSE(parts[2].empty()); //盐
    EXPECT_FALSE(parts[3].empty()); //散列

    EXPECT_TRUE(password_hash::is_encoded(encoded));
}

TEST(PasswordHash, SamePasswordYieldsDifferentHashes)
{
    //逐条随机的盐使相同口令在库中呈现出不同散列
    const std::string first = password_hash::encode("same-password");
    const std::string second = password_hash::encode("same-password");
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);

    //但两者都能校验通过
    EXPECT_TRUE(password_hash::verify("same-password", first));
    EXPECT_TRUE(password_hash::verify("same-password", second));
}

TEST(PasswordHash, VerifiesOnlyTheMatchingPassword)
{
    const std::string encoded = password_hash::encode("s3cret");
    ASSERT_FALSE(encoded.empty());

    EXPECT_TRUE(password_hash::verify("s3cret", encoded));
    EXPECT_FALSE(password_hash::verify("s3cres", encoded)); //末位不同
    EXPECT_FALSE(password_hash::verify("S3cret", encoded)); //大小写不同
    EXPECT_FALSE(password_hash::verify("", encoded));
}

TEST(PasswordHash, PlaintextStoredValueNeverVerifies)
{
    //历史遗留的明文行必须被判为不匹配，否则等于绕过了哈希
    EXPECT_FALSE(password_hash::verify("123456", "123456"));
    EXPECT_FALSE(password_hash::verify("", ""));
    EXPECT_FALSE(password_hash::is_encoded("123456"));
    EXPECT_FALSE(password_hash::is_encoded(""));
}

TEST(PasswordHash, RejectsMalformedStoredValues)
{
    const std::string good = password_hash::encode("pw");
    ASSERT_FALSE(good.empty());

    EXPECT_FALSE(password_hash::verify("pw", ""));                                   //空
    EXPECT_FALSE(password_hash::verify("pw", "pw"));                                 //无分隔符
    EXPECT_FALSE(password_hash::verify("pw", "pbkdf2_sha256$100000$"));              //字段不足
    EXPECT_FALSE(password_hash::verify("pw", "bcrypt$100000$AAAA$AAAA"));            //算法不符
    EXPECT_FALSE(password_hash::verify("pw", with_iterations(good, "abc")));         //迭代次数非数字
    EXPECT_FALSE(password_hash::verify("pw", with_iterations(good, "0")));           //迭代次数为 0
    EXPECT_FALSE(password_hash::verify("pw", with_iterations(good, "99999999999"))); //迭代次数过大
    EXPECT_FALSE(password_hash::verify("pw", with_iterations(good, "-1")));          //迭代次数为负

    //散列字段换成 16 字节的 base64（24 字符），与 32 字节的推导结果长度不符。
    //此处不能靠改写末尾字符来构造：base64 末组中不参与解码的低位被改变时，
    //解出的字节可能与原值完全相同，那样的用例会时灵时不灵
    const std::string short_hash = "AAAAAAAAAAAAAAAAAAAAAA==";
    EXPECT_FALSE(password_hash::verify("pw", good.substr(0, good.rfind('$') + 1) + short_hash));
    EXPECT_FALSE(password_hash::verify("pw", good.substr(0, good.rfind('$') + 1))); //散列字段为空
}

TEST(PasswordHash, UsesIterationCountFromStoredValue)
{
    //迭代次数取自记录本身，因此改动它会得到一个不同的散列——
    //若实现忽略记录中的次数而固定使用常量，这里就会误判为通过
    const std::string good = password_hash::encode("pw");
    ASSERT_FALSE(good.empty());

    const std::string tampered = with_iterations(good, "1000");
    EXPECT_NE(tampered, good);
    EXPECT_FALSE(password_hash::verify("pw", tampered));
}

TEST(PasswordHash, HandlesEmptyAndLongPasswords)
{
    const std::string empty = password_hash::encode("");
    ASSERT_FALSE(empty.empty());
    EXPECT_TRUE(password_hash::verify("", empty));
    EXPECT_FALSE(password_hash::verify("x", empty));

    const std::string long_password(1000, 'p');
    const std::string encoded = password_hash::encode(long_password);
    ASSERT_FALSE(encoded.empty());
    EXPECT_TRUE(password_hash::verify(long_password, encoded));
    EXPECT_FALSE(password_hash::verify(std::string(999, 'p'), encoded));
}
} // namespace
