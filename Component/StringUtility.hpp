
#pragma once

#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <vector>

template <typename Str>
struct tokenizer
{
    tokenizer(Str const& str)
        : _string(str), _offset(0)
    {}
    tokenizer(Str const& str, Str const& delimiters)
        : _string(str), _offset(0), _delimiters(delimiters)
    {}

    bool next_token()
    {
        return next_token(_delimiters);
    }

    bool next_token(Str const& delimiters)
    {
        size_t i = _string.find_first_not_of(delimiters, _offset);
        if (i == Str::npos)
        {
            _offset = _string.length();
            return false;
        }

        size_t j = _string.find_first_of(delimiters, i);
        if (j == Str::npos) {
            _token = _string.substr(i);
            _offset = _string.length();
            return true;
        }

        _token = _string.substr(i, j - i);
        _offset = j;
        return true;
    }

    const Str get_token() const
    {
        return _token;
    }

    void reset()
    {
        _offset = 0;
    }

    size_t _offset;
    const Str _string;
    Str _token;
    Str _delimiters;
};

template <typename Str>
struct StringUtility
{

    static Str ToUpper(const Str& str)
    {
        Str temp(str);
        std::transform(temp.begin(), temp.end(), temp.begin(), ::toupper);
        return temp;
    }

    static Str ToLower(const Str& str)
    {
        Str temp(str);
        std::transform(temp.begin(), temp.end(), temp.begin(), ::tolower);
        return temp;
    }

    static Str TrimLeft(const Str& str)
    {
        Str temp(str);
        auto it = temp.begin();
        for (it = temp.begin(); it != temp.end(); it++) {
            if (!isspace(*it)) {
                break;
            }
        }
        if (it == temp.end()) {
            temp.clear();
        } else {
            temp.erase(temp.begin(), it);
        }
        return temp;
    }

    static Str TrimRight(const Str& str)
    {
        Str temp(str);
        for (auto it = temp.end() - 1; ;it--) {
            if (!isspace(*it)) {
                temp.erase(it + 1, temp.end());
                break;
            }
            if (it == temp.begin()) {
                temp.clear();
                break;
            }
        }
        return temp;
    }

    static Str Trim(const Str& str)
    {
        Str temp = trim_left(str);
        return trim_right(temp);
    }

    static bool StartsWith(Str const & value, Str const & starting)
    {
        if (starting.size() > value.size()) return false;
        return std::equal(starting.begin(), starting.end(), value.begin());
    }

    static bool EndsWith(Str const & value, Str const & ending)
    {
        if (ending.size() > value.size()) return false;
        return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
    }

    static bool EqualsIgnoreCase(const Str& str1, const Str& str2)
    {
        return to_lower(str1) == to_lower(str2);
    }

    static bool IStartsWith(Str const & value, Str const & starting)
    {
        if (starting.size() > value.size()) return false;
        Str temp = value.substr(0, starting.size());
        return equals_ignore_case(to_lower(starting), to_lower(temp));
    }

    static bool IEndsWith(Str const & value, Str const & ending)
    {
        if (ending.size() > value.size()) return false;
        Str temp = value.substr(value.size() - ending.size(), ending.size());
        return equals_ignore_case(to_lower(ending), to_lower(temp));
    }

    template <typename T>
    static T FromString(const Str& str)
    {
        T obj;
        std::basic_istringstream<typename Str::value_type> temp(str);
        temp >> obj;
        return obj;
    }

    static bool FromString(const Str& str)
    {
        bool obj;
        std::basic_istringstream<typename Str::value_type> temp(str);
        temp >> std::boolalpha >> obj;
        return obj;
    }

    template <typename T>
    static T FromHexString(const Str& str)
    {
        T obj;
        std::basic_istringstream<typename Str::value_type> temp(str);
        temp >> std::hex >> obj;
        return obj;
    }

    template <typename T>
    static Str ToString(const T& var)
    {
        std::basic_ostringstream<typename Str::value_type> temp;
        temp << var;
        return temp.str();
    }

    static Str ToString(const bool& var)
    {
        std::basic_ostringstream<typename Str::value_type> temp;
        temp << std::boolalpha << var;
        return temp.str();
    }

    template <typename T>
    static Str ToHexString(const T& var, int width)
    {
        std::basic_ostringstream<typename Str::value_type> temp;
        temp << std::hex;
        if(width > 0)
        {
            temp << std::setw(width) << std::setfill<typename Str::value_type>('0');
        }
        temp << var;
        return temp.str();
    }

    static std::vector<Str> Split(Str const& str, Str const& delimiters)
    {
        std::vector<Str> ss;
        tokenizer<Str> token(str, delimiters);
        while (token.next_token())
        {
            ss.push_back(token.get_token());
        }
        return ss;
    }

};

typedef StringUtility<std::string> StringUtilityA;
typedef StringUtility<std::wstring> StringUtilityW;

