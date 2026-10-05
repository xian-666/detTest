#pragma once
// AppConfig.h
//
// UTF-8 aware INI reader for detTest.
//
// Why not GetPrivateProfileStringW()?
//   The private-profile API interprets a non-UTF-16 file using the system ANSI
//   code page (GBK on a Chinese Windows). config.ini is UTF-8, so every Chinese
//   window title would come back mojibake. This reader decodes as UTF-8 first and
//   then parses, which makes the result independent of the machine locale.
//
// Recognised syntax:
//   [Section]            case-insensitive
//   key = value          ':' is accepted as a separator too
//   key                  bare key -> value L"1"
//   ; comment  # comment whole-line comments only, so values may contain '#'
//   value                surrounding double quotes are stripped

#include <map>
#include <string>
#include <utility>
#include <windows.h>

namespace strconv {
    // UTF-16 <-> UTF-8 helpers. Used for logging wide config values to stdout
    // and for writing them into the CSV report.
    std::string  ToUtf8(const std::wstring& wide);
    std::wstring ToWide(const std::string& utf8);
}

namespace appcfg {

class IniFile {
public:
    // Reads a UTF-8 encoded file. Returns false when the file is unreadable or
    // undecodable. On failure the object stays empty and every getter returns
    // the caller supplied default.
    bool Load(const std::wstring& path);
    bool IsLoaded() const { return m_loaded; }

    std::wstring GetString(const std::wstring& section, const std::wstring& key,
                           const std::wstring& def = L"") const;
    bool  GetBool (const std::wstring& section, const std::wstring& key, bool def = false) const;
    int   GetInt  (const std::wstring& section, const std::wstring& key, int   def = 0) const;
    double GetDouble(const std::wstring& section, const std::wstring& key, double def = 0.0) const;

private:
    using Key = std::pair<std::wstring, std::wstring>;  // (lowercased section, lowercased key)
    std::map<Key, std::wstring> m_values;
    bool m_loaded = false;
};

}  // namespace appcfg
