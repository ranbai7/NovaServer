#ifndef URL_CODEC_H
#define URL_CODEC_H

#include <string>
#include <vector>

//URL 转义解码与路径规范化。两个函数都只做纯字符串变换，不依赖连接状态与全局配置，故放在头文件
//中以便直接单元测试——路径规范化是安全边界所在，仅靠端到端测试难以覆盖「编码后被绕过」一类输入组合
namespace url_codec
{
//十六进制字符取值，非十六进制字符返回 -1
inline int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

//解码 %XX 转义。plus_as_space 为真时把 '+' 还原为空格（表单请求体与查询串的约定），
//路径中的 '+' 是字面量不参与还原；非法转义（%ZZ、结尾不完整的 %A）按字面量保留
inline std::string decode(const std::string &in, bool plus_as_space)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        else if (in[i] == '+' && plus_as_space)
        {
            out.push_back(' ');
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

//把路径规范化为「以 '/' 开头、不含空段与 . 与 .. 段」的形式：空段（重复的 '/'）与 '.' 段
//丢弃，'..' 回退上一级；回退层数超过已有层级说明试图越过根目录，此时返回空串。
//调用方必须先用 decode 处理 %XX：%2e%2e%2f 这类编码解码后才是 '..'，先规范化后解码会让编码形式绕过全部判断
inline std::string normalize_path(const std::string &path)
{
    std::vector<std::string> segments;
    size_t i = 0;
    while (i < path.size())
    {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos)
            slash = path.size();

        const std::string segment = path.substr(i, slash - i);
        if (segment.empty() || segment == ".")
        {
            //空段与当前目录段：直接跳过
        }
        else if (segment == "..")
        {
            if (segments.empty())
                return std::string(); //试图越过根目录
            segments.pop_back();
        }
        else
        {
            segments.push_back(segment);
        }
        i = slash + 1;
    }

    std::string out;
    for (const std::string &segment : segments)
    {
        out += '/';
        out += segment;
    }
    if (out.empty())
        out = "/";
    return out;
}
} // namespace url_codec

#endif
