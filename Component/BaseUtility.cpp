#include "BaseUtility.hpp"
#include <Windows.h>
#include <array>
#include <shlwapi.h>
namespace BaseUtility {
    // Function to create a directory
    bool CreateDirectory_(const std::string& path) {
        try {
            if (std::filesystem::create_directory(path)) {
                std::cout << "Directory created: " << path << std::endl;
                return true;
            }
            else {
                std::cout << "Directory already exists or cannot be created: " << path << std::endl;
                return false;
            }
        }
        catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
            return false;
        }
    }

    // Function to check if a directory exists
    bool CheckDirectoryExist(const std::string& path) {
        return std::filesystem::exists(path) && std::filesystem::is_directory(path);
    }

    // Function to get the current working directory
    std::string GetCurrentDirectory_() {
        try {
            std::string detectionExeDir = BaseUtility::GetAppPathA();
            return detectionExeDir;
        }
        catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error getting current directory: " << e.what() << std::endl;
            return std::string();
        }
    }
    std::wstring String2WString(std::string str)
    {
        std::wstring result;
        //获取缓冲区大小，并申请空间，缓冲区大小按字符计算  
        int len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.size(), NULL, 0);
        TCHAR* buffer = new TCHAR[len + 1];
        //多字节编码转换成宽字节编码  
        MultiByteToWideChar(CP_ACP, 0, str.c_str(), str.size(), buffer, len);
        buffer[len] = '\0';             //添加字符串结尾  
        //删除缓冲区并返回值  
        result.append(buffer);
        delete[] buffer;
        return result;
    }
    bool CheckFileExist(std::string strPath) {
        return std::filesystem::exists(strPath) && std::filesystem::is_regular_file(strPath);
    }

    std::string Exec(const char* cmd) {
        //AllocConsole();    //为调用进程分配一个新的控制台
        //ShowWindow(GetConsoleWindow(), SW_HIDE);    //隐藏自己创建的控制台
        std::array<char, 128> buffer;
        std::string result;
        FILE* pipe = _popen(cmd, "r");  // Use _popen on Windows
        if (!pipe) {
            throw std::runtime_error("_popen() failed!");
        }
        try {
            while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                result += buffer.data();
            }
        }
        catch (...) {
            _pclose(pipe);  // Use _pclose on Windows
            throw;
        }
        _pclose(pipe);  // Use _pclose on Windows
        return result;
    }

    bool IsNumber(const std::string& s)
    {
        return !s.empty() && std::find_if(s.begin(),
            s.end(), [](unsigned char c) { return !std::isdigit(c); }) == s.end();
    }

    //获取当前时间戳
    std::string getCurrentDateTime()
    {
        auto now = std::chrono::system_clock::now();
        // 转换为time_t类型
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        // 将time_t转换为tm结构体，用于格式化输出
        std::tm now_tm;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&now_tm, &now_time_t);
#else
        localtime_r(&now_time_t, &now_tm);
#endif
        // 使用ostringstream进行格式化
        std::ostringstream oss;
        oss << std::put_time(&now_tm, "%Y_%m_%d_%H_%M_%S");
        return oss.str();
    }

    //字符串分割
    std::vector<std::string> stringSplit(std::string str, char split)
    {
        std::istringstream iss(str);    // 输入流
        std::string token;            // 接收缓冲区
        std::vector<std::string> result;
        while (getline(iss, token, split))    // 以split为分隔符
        {
            result.push_back(token);
        }
        return result;
    }

    //判断文件夹是否存在
    bool directoryExists(const std::string& path) {
        std::filesystem::path directory(path);
        return std::filesystem::exists(directory) && std::filesystem::is_directory(directory);
    }

    //新建文件夹
    bool createDirectory(const std::string& path) {
        std::filesystem::path directory(path);
        char split = '\\';
        if (path.find("\\") != std::string::npos) {
            split = '\\';
        }
        else {
            split = '/';
        }
        std::vector<std::string> dirItem = stringSplit(path, split);
        std::string currentPath;
        for (auto const& item : dirItem) {
            currentPath += item + split;
            if (!directoryExists(currentPath)) {
                try {
                    if (!std::filesystem::create_directory(currentPath)) {
                        return false;
                    }
                }
                catch (const std::filesystem::filesystem_error& e) {
                    return false;
                }
            }
        }
        return true;
    }

    std::string WString2String(std::wstring wstr)
    {
        std::string result;
        //获取缓冲区大小，并申请空间，缓冲区大小事按字节计算的  
        int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.size(), NULL, 0, NULL, NULL);
        char* buffer = new char[len + 1];
        //宽字节编码转换成多字节编码  
        WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), wstr.size(), buffer, len, NULL, NULL);
        buffer[len] = '\0';
        //删除缓冲区并返回值  
        result.append(buffer);
        delete[] buffer;
        return result;
    }
    std::string GetTimeStampString()
    {       
        std::time_t timestamp = std::time(nullptr);// 获取当前时间的时间戳
        std::tm* now_tm = std::localtime(&timestamp);// 使用 localtime 或 gmtime 将时间戳转换为本地时间或格林威治时间结构体      
        char buffer[30];// 创建一个字符数组用于存储转换后的日期字符串
        // 设置时间格式
        if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d-%H-%M-%S", now_tm) > 0) {
            std::string formattedTime(buffer);
            return formattedTime;
        }
        else
            return "default_default";
    }
    void printVector(const std::vector<uint8_t>& vec)
    {
        for (size_t i = 0; i < vec.size(); ++i) {
            std::cout << "0x" << std::setw(2) << std::setfill('0') << static_cast<int>(vec[i]) << " ";
        }
        std::cout << std::endl; // Print a newline at the end
    }
    std::string GetForegroundWindowTitle()
    {
        // 获取当前活动窗口句柄
        HWND hwnd = GetForegroundWindow();
        if (hwnd == nullptr) {
            return "No active window";
        }

        // 获取窗口标题长度
        int length = GetWindowTextLength(hwnd);
        if (length == 0) {
            return "Untitled";
        }

        // 获取窗口标题
        char* buffer = new char[length + 1];
        GetWindowTextA(hwnd, buffer, length + 1);

        std::string windowTitle(buffer);
        delete[] buffer;

        return windowTitle;
    }

    // 获取应用程序路径
    std::string GetAppPathA() {
        char szPath[MAX_PATH] = { 0 };
        GetModuleFileNameA(NULL, szPath, MAX_PATH);
        PathRemoveFileSpecA(szPath);
        return std::string(szPath);
    }

    std::string GetAppPathA(int level)
    {
        char szPath[MAX_PATH] = { 0 };
        GetModuleFileNameA(NULL, szPath, MAX_PATH);
        // 循环回退level级目录
        for (int i = 0; i < level; ++i) {
            if (!PathRemoveFileSpecA(szPath)) {
                // 已经到顶层了，不能再回退
                break;
            }
        }
        return std::string(szPath);
    }
    void fun_truck(double& num, int k)
    {
        int pk = (int)pow(10, k);      //除数
        int zr = int(num * pk);
        int z = zr / pk;               //整数部分
        int r = zr % pk;               //小数部分

        num = z + r * 0.000001;          //大数吃小数
    }
}



