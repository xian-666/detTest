#ifndef BASE_UTILITY_HPP
#define BASE_UTILITY_HPP

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

namespace BaseUtility {
    // Function to create a directory
    bool CreateDirectory_(const std::string& path);

    // Function to check if a directory exists
    bool CheckDirectoryExist(const std::string& path);

    // Function to get the current working directory
    std::string GetCurrentDirectory_();

    std::wstring String2WString(std::string str);

    bool CheckFileExist(std::string strPath);

    std::string Exec(const char* cmd);

    bool IsNumber(const std::string& s);

    //function to get timestamp
    std::string getCurrentDateTime();

    //std::vector<std::string> stringSplit(std::string str, char split);

    bool directoryExists(const std::string& path);

    bool createDirectory(const std::string& path);

    std::string WString2String(std::wstring wstr);

    std::string GetTimeStampString();

    void printVector(const std::vector<uint8_t>& vec);

    std::string GetForegroundWindowTitle();

    std::string GetAppPathA();

    std::string GetAppPathA(int level);

    //截断k位小数，返回ans
    void fun_truck(double& num, int k);


}

#endif // BASE_UTILITY_HPP
