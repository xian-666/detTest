#include "AppConfig.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace {

std::wstring LowerCopy(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return s;
}

void TrimInPlace(std::wstring& s) {
    auto space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v';
    };
    size_t i = 0, j = s.size();
    while (i < j && space(s[i])) ++i;
    while (j > i && space(s[j - 1])) --j;
    s.assign(s, i, j - i);
}

}  // namespace

namespace strconv {

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        &out[0], need, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                     static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                        static_cast<int>(utf8.size()), &out[0], need);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

}  // namespace strconv

namespace appcfg {

bool IniFile::Load(const std::wstring& path) {
    m_values.clear();
    m_loaded = false;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) return false;

    std::vector<char> raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (raw.empty()) {
        m_loaded = true;
        return true;
    }

    size_t offset = 0;
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        offset = 3;
    }

    std::wstring text = strconv::ToWide(std::string(raw.begin() + offset, raw.end()));
    if (text.empty()) return false;

    std::wstring section;
    std::wstring line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t next = text.find(L'\n', pos);
        line.assign(text, pos, (next == std::wstring::npos ? text.size() : next) - pos);
        TrimInPlace(line);

        if (!line.empty() && line[0] != L';' && line[0] != L'#') {
            if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
                section = LowerCopy(line.substr(1, line.size() - 2));
                TrimInPlace(section);
            } else {
                size_t sep = line.find_first_of(L"=:");
                std::wstring key = (sep == std::wstring::npos) ? line : line.substr(0, sep);
                std::wstring val = (sep == std::wstring::npos) ? L"1" : line.substr(sep + 1);
                TrimInPlace(key);
                TrimInPlace(val);
                if (!key.empty()) {
                    // Strip one layer of matching double quotes.
                    if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"') {
                        val = val.substr(1, val.size() - 2);
                    }
                    m_values[{section, LowerCopy(key)}] = val;
                }
            }
        }

        if (next == std::wstring::npos) break;
        pos = next + 1;
    }

    m_loaded = true;
    return true;
}

std::wstring IniFile::GetString(const std::wstring& section, const std::wstring& key,
                                const std::wstring& def) const {
    auto it = m_values.find({LowerCopy(section), LowerCopy(key)});
    return (it == m_values.end()) ? def : it->second;
}

bool IniFile::GetBool(const std::wstring& section, const std::wstring& key, bool def) const {
    std::wstring v = GetString(section, key, def ? L"true" : L"false");
    std::wstring lower = LowerCopy(v);
    if (lower == L"1" || lower == L"true" || lower == L"yes" || lower == L"on") return true;
    if (lower == L"0" || lower == L"false" || lower == L"no" || lower == L"off") return false;
    return def;
}

int IniFile::GetInt(const std::wstring& section, const std::wstring& key, int def) const {
    std::wstring v = GetString(section, key);
    if (v.empty()) return def;
    try {
        size_t consumed = 0;
        long long out = std::stoll(v, &consumed, 0);
        if (consumed == 0) return def;
        return static_cast<int>(out);
    } catch (...) {
        return def;
    }
}

double IniFile::GetDouble(const std::wstring& section, const std::wstring& key, double def) const {
    std::wstring v = GetString(section, key);
    if (v.empty()) return def;
    try {
        size_t consumed = 0;
        double out = std::stod(v, &consumed);
        if (consumed == 0) return def;
        return out;
    } catch (...) {
        return def;
    }
}

}  // namespace appcfg
