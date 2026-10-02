#include "password_hash.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdlib>
#include <cstring>

namespace
{
//迭代次数。该值随散列一同存储，因此调整它不会使既有记录失效
const int kIterations = 100000;
const int kSaltBytes = 16;
const int kKeyBytes = 32; //SHA-256 的输出长度
const char kAlgorithm[] = "pbkdf2_sha256";
//迭代次数的合理上界：防止被篡改的记录让校验陷入长时间计算
const long kMaxIterations = 10000000;

std::string base64_encode(const unsigned char *data, size_t len)
{
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]), data, static_cast<int>(len));
    if (written < 0)
        return std::string();
    out.resize(static_cast<size_t>(written));
    return out;
}

//base64 解码，并去掉填充字符贡献的长度
bool base64_decode(const std::string &text, std::string &out)
{
    if (text.empty() || text.size() % 4 != 0)
        return false;

    out.assign(text.size() / 4 * 3, '\0');
    //EVP_DecodeBlock 不会改写输入，去掉 const 仅为匹配它的签名
    const int written =
        EVP_DecodeBlock(reinterpret_cast<unsigned char *>(&out[0]),
                        const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(text.data())),
                        static_cast<int>(text.size()));
    if (written < 0)
        return false;

    //解码结果按 4 字节对齐，填充字符也计入长度，逐个减掉
    size_t length = static_cast<size_t>(written);
    for (size_t i = text.size(); i > 0 && text[i - 1] == '='; --i)
        --length;
    out.resize(length);
    return true;
}

//按给定盐与迭代次数推导密钥
bool derive(const std::string &password, const std::string &salt, int iterations, std::string &key)
{
    key.assign(kKeyBytes, '\0');
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                             reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
                             iterations, EVP_sha256(), kKeyBytes, reinterpret_cast<unsigned char *>(&key[0])) == 1;
}
} // namespace

std::string password_hash::encode(const std::string &password)
{
    unsigned char salt[kSaltBytes];
    if (RAND_bytes(salt, sizeof(salt)) != 1)
        return std::string();

    const std::string salt_bytes(reinterpret_cast<char *>(salt), sizeof(salt));
    std::string key;
    if (!derive(password, salt_bytes, kIterations, key))
        return std::string();

    const std::string salt_text = base64_encode(salt, sizeof(salt));
    const std::string key_text = base64_encode(reinterpret_cast<const unsigned char *>(key.data()), key.size());
    if (salt_text.empty() || key_text.empty())
        return std::string();

    return std::string(kAlgorithm) + "$" + std::to_string(kIterations) + "$" + salt_text + "$" + key_text;
}

bool password_hash::verify(const std::string &password, const std::string &encoded)
{
    //按 '$' 切成「算法 / 迭代次数 / 盐 / 散列」四段
    const size_t first = encoded.find('$');
    if (first == std::string::npos)
        return false;
    const size_t second = encoded.find('$', first + 1);
    if (second == std::string::npos)
        return false;
    const size_t third = encoded.find('$', second + 1);
    if (third == std::string::npos)
        return false;

    const std::string algorithm = encoded.substr(0, first);
    if (algorithm != kAlgorithm)
        return false;

    const std::string iterations_text = encoded.substr(first + 1, second - first - 1);
    char *end = nullptr;
    const long iterations = std::strtol(iterations_text.c_str(), &end, 10);
    if (end == iterations_text.c_str() || *end != '\0' || iterations <= 0 || iterations > kMaxIterations)
        return false;

    std::string salt;
    std::string expected;
    if (!base64_decode(encoded.substr(second + 1, third - second - 1), salt))
        return false;
    if (!base64_decode(encoded.substr(third + 1), expected))
        return false;

    std::string key;
    if (!derive(password, salt, static_cast<int>(iterations), key))
        return false;
    if (expected.size() != key.size())
        return false;

    //定长比较，不因首个不同字节而提前返回
    return CRYPTO_memcmp(expected.data(), key.data(), expected.size()) == 0;
}

bool password_hash::is_encoded(const std::string &stored)
{
    return stored.compare(0, std::strlen(kAlgorithm), kAlgorithm) == 0;
}
