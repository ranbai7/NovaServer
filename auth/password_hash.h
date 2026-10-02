#ifndef PASSWORD_HASH_H
#define PASSWORD_HASH_H

#include <string>

//口令的加盐哈希。存储格式为「算法$迭代次数$盐$散列」，字段以 '$' 分隔（base64 字母表
//不含 '$'，故按它切分是安全的）。盐与迭代次数随散列一并保存：盐逐条随机，使相同口令在
//库中呈现不同散列、彩虹表失效；迭代次数写在记录里，日后提高代价参数时旧记录仍按自身记录
//的次数校验。散列用 PBKDF2-HMAC-SHA256：它是专为口令设计的慢哈希，迭代次数把「单次校验耗时」抬到可接受、而「离线穷举」不可接受
namespace password_hash
{
//把明文口令编码为可入库的哈希串。随机数不可用时返回空串
std::string encode(const std::string &password);

//校验明文口令与已存储的哈希串是否匹配。
//存储值不是本模块的编码格式时一律返回 false
bool verify(const std::string &password, const std::string &encoded);

//判断存储值是否已是哈希格式，用于识别历史遗留的明文记录
bool is_encoded(const std::string &stored);
} // namespace password_hash

#endif
