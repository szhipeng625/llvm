// ============================================================================
// kv.add / kv.build 的 args 参数解析器
//
// 兼容三种输入格式：
//   1. 标准 JSON 字符串数组:  ["arg1","arg2"]   （推荐）
//   2. 标准 JSON 值数组:      [300] [8890, "x"] （数字/字符串混用）
//   3. 硬编码裸方括号(旧写法): [/path/to.py] [--flag value]
//
// 规则：
//   - 优先按【逗号】切分 JSON 数组元素；
//   - 每个元素：有引号 → 取引号内字符串；无引号 → 取连续非分隔字符；
//   - 若整体不含逗号（说明是单一裸参数，如 [/path/to.py]）→ 去掉首尾
//     方括号后作为【一个】整体参数（兼容硬编码老写法）。
// ============================================================================
#ifndef PYLITE_KV_ARGS_HELPER_H
#define PYLITE_KV_ARGS_HELPER_H

#include <string>
#include <vector>

namespace pylite {

inline std::vector<std::string> parse_kv_args(const std::string& raw) {
    std::vector<std::string> out;

    // 去掉首尾空白
    size_t b = 0, e = raw.size();
    while (b < e && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\n' || raw[b] == '\r')) b++;
    while (e > b && (raw[e-1] == ' ' || raw[e-1] == '\t' || raw[e-1] == '\n' || raw[e-1] == '\r')) e--;
    std::string s = raw.substr(b, e - b);
    if (s.empty()) return out;

    // 去掉最外层方括号（若存在）
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }

    // 情况 1/2：含逗号 → 按逗号切分 JSON 数组元素
    if (s.find(',') != std::string::npos) {
        size_t i = 0;
        while (i < s.size()) {
            // 跳过空白与逗号
            while (i < s.size() && (s[i] == ',' || s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i >= s.size()) break;
            std::string elem;
            if (s[i] == '"') {
                // 引号字符串：取到配对引号
                size_t j = s.find('"', i + 1);
                if (j == std::string::npos) {
                    elem = s.substr(i + 1);
                    i = s.size();
                } else {
                    elem = s.substr(i + 1, j - i - 1);
                    i = j + 1;
                }
            } else {
                // 无引号（数字/裸词）：取到逗号或空白
                size_t j = i;
                while (j < s.size() && s[j] != ',' && s[j] != ' ' && s[j] != '\t') j++;
                elem = s.substr(i, j - i);
                i = j;
            }
            // 空字符串元素（"" → 空实参）也不再特殊处理，直接压入
            out.push_back(elem);
        }
        return out;
    }

    // 情况 3：不含逗号 → 单一裸参数（兼容 [/path/to.py] / [--flag value]）
    // 尝试引号优先；否则整个当作一个参数
    std::string trim = s;
    size_t tb = 0, te = trim.size();
    while (tb < te && (trim[tb]==' '||trim[tb]=='\t')) tb++;
    while (te > tb && (trim[te-1]==' '||trim[te-1]=='\t')) te--;
    trim = trim.substr(tb, te - tb);
    if (!trim.empty()) out.push_back(trim);
    return out;
}

} // namespace pylite

#endif // PYLITE_KV_ARGS_HELPER_H
