#include "DumpHelper.h"
#include "../Component/BaseUtility.hpp"
#include <Windows.h>
#include "time.h"
#include <vector>
#include <DbgHelp.h>
#pragma comment(lib,"Dbghelp.lib")
#ifdef _WIN32
#include <io.h>
#include <direct.h> 
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <stdint.h>
#include <string>
#include <iostream>

#define MAX_PATH_LEN 256
#ifdef _WIN32
#define ACCESS(fileName,accessMode) _access(fileName,accessMode)
#define MKDIR(path) _mkdir(path)
#else
#define ACCESS(fileName,accessMode) access(fileName,accessMode)
#define MKDIR(path) mkdir(path,S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)
#endif
namespace AC {
	static std::string _directory = BaseUtility::GetCurrentDirectory_();
	
	static int _fileCount = 500;
	//获取文件夹下的所有文件名
	static void GetFolderFiles(const std::string& path, std::vector<std::string>& files)
	{
		//文件句柄  
		intptr_t hFile = 0;
		//文件信息  
		struct _finddata_t fileinfo;
		std::string p;
		if ((hFile = _findfirst(p.assign(path).append("\\*").c_str(), &fileinfo)) != -1)
			// "\\*"是指读取文件夹下的所有类型的文件，若想读取特定类型的文件，以png为例，则用“\\*.png”
		{
			do
			{
				//如果是目录,迭代之  
				//如果不是,加入列表  
				if ((fileinfo.attrib & _A_SUBDIR))
				{
					if (strcmp(fileinfo.name, ".") != 0 && strcmp(fileinfo.name, "..") != 0)
						GetFolderFiles(p.assign(path).append("\\").append(fileinfo.name), files);
				}
				else
				{
					files.push_back(path + "\\" + fileinfo.name);
				}
			} while (_findnext(hFile, &fileinfo) == 0);
			_findclose(hFile);
		}
	}
	//生成多级目录，中间目录不存在则自动创建
	static bool CreateMultiLevelDirectory(const std::string& directoryPath) {
		uint32_t dirPathLen = directoryPath.length();
		if (dirPathLen > MAX_PATH_LEN)
		{
			return false;
		}
		char tmpDirPath[MAX_PATH_LEN] = { 0 };
		for (uint32_t i = 0; i < dirPathLen; ++i)
		{
			tmpDirPath[i] = directoryPath[i];
			if (tmpDirPath[i] == '\\' || tmpDirPath[i] == '/')
			{
				if (ACCESS(tmpDirPath, 0) != 0)
				{
					int32_t ret = MKDIR(tmpDirPath);
					if (ret != 0)
					{
						return false;
					}
				}
			}
		}
		return true;
	}
	/// <summary>
	/// 生成dmp文件
	/// </summary>
	/// <param name="exceptionPointers">异常信息</param>
	/// <param name="path">文件路径（包括文件名）</param>
	/// <returns></returns>
	static int GenerateDump(EXCEPTION_POINTERS* exceptionPointers, const std::string& path)
	{
		HANDLE hFile = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE != hFile)
		{
			MINIDUMP_EXCEPTION_INFORMATION minidumpExceptionInformation;
			minidumpExceptionInformation.ThreadId = GetCurrentThreadId();
			minidumpExceptionInformation.ExceptionPointers = exceptionPointers;
			minidumpExceptionInformation.ClientPointers = TRUE;
			bool isMiniDumpGenerated = MiniDumpWriteDump(
				GetCurrentProcess(),
				GetCurrentProcessId(),
				hFile,
				MINIDUMP_TYPE::MiniDumpNormal,
				&minidumpExceptionInformation,
				nullptr,
				nullptr);

			CloseHandle(hFile);
			if (!isMiniDumpGenerated)
			{
				printf("MiniDumpWriteDump failed\n");
			}
		}
		else
		{
			printf("Failed to create dump file\n");
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}
	//全局异常捕获回调
	static LONG WINAPI ExceptionFilter(LPEXCEPTION_POINTERS lpExceptionInfo)
	{
		int i = 0;
		time_t t;
		struct tm* local;
		char path[1024] = { 0 };
		char ext[MAX_PATH] = { 0 };
		t = time(NULL);
		local = localtime(&t);
		//创建目录
		if (!CreateMultiLevelDirectory(_directory))
		{
			printf("Failed to create directory %s\n", _directory.c_str());
			return EXCEPTION_EXECUTE_HANDLER;
		}
		//清除多出的文件
		std::vector<std::string> files;
		std::vector<std::string> dmpFiles;
		GetFolderFiles(_directory, files);
		for (auto i : files)
		{
			_splitpath(i.c_str(), NULL, NULL, NULL, ext);
			if (strcmp(ext, "dmp") == 0)
			{
				dmpFiles.push_back(i);
			}
		}
		if (dmpFiles.size() >= _fileCount)
		{
			if (strcmp(ext, "dmp") == 0 && !DeleteFileA(dmpFiles.front().c_str()))
			{
				printf("Failed to delete old file %s\n", dmpFiles.front().c_str());
			}
		}
		//生成文件名
		sprintf(path, "%s%d%02d%02d%02d%02d%02d.dmp", _directory.c_str(), 1900 + local->tm_year, local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
		if (strlen(path) > MAX_PATH)
		{
			printf("File path was too long! %s\n", path);
			return EXCEPTION_EXECUTE_HANDLER;
		}
		//生成dmp文件
		return GenerateDump(lpExceptionInfo, path);
	}
	void DumpHelper::SetIsDumpCrash(bool value)
	{
		if (value)
			SetUnhandledExceptionFilter(ExceptionFilter);
		else
		{
			SetUnhandledExceptionFilter(NULL);
		}
	}
	void DumpHelper::SetDumpDirectory(const std::string& directory)
	{
		_directory = directory;
		if (_directory.size() < 1)
		{
			_directory = ".\\";
		}
		else if (_directory.back() != '\\')
		{
			_directory.push_back('\\');
		}
	}
	void DumpHelper::SetDumpMaxFileCount(int count)
	{
		_fileCount = count;
	}
	void DumpHelper::Snapshot(const std::string& path)
	{
		__try
		{
			//通过触发异常获取堆栈
			RaiseException(0xE0000001, 0, 0, 0);
		}
		__except (GenerateDump(GetExceptionInformation(), path)) {}
	}
}
