#pragma once
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <filesystem>
#include "BaseUtility.hpp"

struct LogTarget {
    std::atomic<bool> logFullImage{ false };
    std::atomic<bool> logDetectionImage{ false };
    std::atomic<bool> logText{ false };
    std::atomic<bool> showDetection{ false };
    std::atomic<bool> logConsole{ false };
};


class LogConfigReader {
public:
    // 读取文件并一次性应用到 target。返回是否成功读取到至少一项。
    static bool LoadFromFile(const std::string& path, LogTarget& target, std::string* err = nullptr) {
        std::wstring wpath = BaseUtility::String2WString(path);
        std::filesystem::path p(wpath);
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs.good()) {
            if (err) *err = "cannot open file: " + path;
            return false;
        }
        return LoadFromStream(ifs, target, err);
    }

    static bool LoadFromFile(const std::string& path, std::string* err = nullptr) {
        std::wstring wpath = BaseUtility::String2WString(path);
        std::filesystem::path p(wpath);
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs.good()) {
            if (err) *err = "cannot open file: " + path;
            return false;
        }
        return LoadFromStream(ifs, m_logTarget, err);
    }

    // 从任意输入流读取（便于单测）
    static bool LoadFromStream(std::istream& is, LogTarget& target, std::string* err = nullptr) {
        // 读取并解析内容，更新 target
        bool hasAny = false;
        bool fullImage = target.logFullImage.load(std::memory_order_acquire);
        bool detImage = target.logDetectionImage.load(std::memory_order_acquire);
        bool text = target.logText.load(std::memory_order_acquire);
        bool show = target.showDetection.load(std::memory_order_acquire);
        bool console = target.logConsole.load(std::memory_order_acquire);
        std::string line;
        size_t lineNo = 0;
        while (std::getline(is, line)) {
            ++lineNo;
            stripComment(line);
            trim(line);
            if (line.empty()) continue;

            auto pos = line.find(':');
            if (pos == std::string::npos) {
                if (err) *err = msg(lineNo, "missing ':'");
                continue;
            }

            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            trim(key); tolower_inplace(key);
            trim(val); tolower_inplace(val);

            bool b;
            if (!parseBool(val, b)) {
                if (err) *err = msg(lineNo, "invalid bool value: '" + val + "'");
                continue;
            }

            if (key == "full_image") { fullImage = b; hasAny = true; }
            else if (key == "detection_image") { detImage = b; hasAny = true; }
            else if (key == "text") { text = b; hasAny = true; }
            else if (key == "show") { show = b; hasAny = true; }
            else if (key == "console") { console = b; hasAny = true; }
            else {
                if (err) *err = msg(lineNo, "unknown key: '" + key + "'");
            }
        }

        target.logFullImage.store(fullImage, std::memory_order_release);
        target.logDetectionImage.store(detImage, std::memory_order_release);
        target.logText.store(text, std::memory_order_release);
        target.showDetection.store(show, std::memory_order_release);
        target.logConsole.store(console, std::memory_order_release);
        return hasAny;
    }

    static LogTarget m_logTarget;
private:
    static void trim(std::string& s) {
        auto issp = [](unsigned char c) { return std::isspace(c); };
        size_t i = 0, j = s.size();
        while (i < j && issp(s[i])) ++i;
        while (j > i && issp(s[j - 1])) --j;
        s.assign(s.substr(i, j - i));
    }
    static void tolower_inplace(std::string& s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static bool parseBool(const std::string& v, bool& out) {
        if (v == "1" || v == "true" || v == "yes" || v == "on") { out = true;  return true; }
        if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
        return false;
    }
    static void stripComment(std::string& s) {
        auto p1 = s.find('#');
        auto p2 = s.find("//");
        size_t p = std::min(p1 == std::string::npos ? s.size() : p1,
            p2 == std::string::npos ? s.size() : p2);
        if (p != std::string::npos) s.erase(p);
    }
    static std::string msg(size_t lineNo, const std::string& what) {
        std::ostringstream os; os << "line " << lineNo << ": " << what; return os.str();
    }
};
