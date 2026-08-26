#pragma once
// twigrpc::env —— 统一配置入口（.env 文件 + 环境变量）。
// 优先级：进程环境变量 > .env 文件 > 调用方默认值；命令行参数由各入口
// 自行解析并覆盖 env 结果（CLI 永远最高）。
// .env 路径：$TWIGRPC_ENV 指定，缺省为工作目录下 .env；文件不存在则静默跳过。
// 格式：每行 KEY=VALUE；# 开头为注释；键值两端空白与成对引号会被去掉。
// 示例见仓库根目录 .env.example。
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace twigrpc::env {

namespace detail {

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// .env 解析一次，进程内只读（C++11 magic static 保证线程安全）
inline const std::unordered_map<std::string, std::string>& dotenv() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        const char* path = std::getenv("TWIGRPC_ENV");
        std::ifstream f((path && *path) ? path : ".env");
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            m.emplace(trim(line.substr(0, eq)),
                      stripQuotes(trim(line.substr(eq + 1))));
        }
        return m;
    }();
    return map;
}

} // namespace detail

// 读字符串配置：环境变量 > .env > def
inline std::string get(const std::string& key, const std::string& def = "") {
    if (const char* v = std::getenv(key.c_str())) return v;
    const auto& m = detail::dotenv();
    auto it = m.find(key);
    return it != m.end() ? it->second : def;
}

// 读整数配置（解析失败/缺省回退 def）
inline int getInt(const std::string& key, int def) {
    try {
        return std::stoi(get(key, ""));
    } catch (...) {
        return def;
    }
}

} // namespace twigrpc::env
