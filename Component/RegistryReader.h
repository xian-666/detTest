#pragma once
#include <Windows.h>
#include <map>
#include <string>
class RegistryReader
{
private:
	RegistryReader();

public:
	RegistryReader(const RegistryReader&) = delete;
	RegistryReader& operator=(const RegistryReader&) = delete;
	~RegistryReader() {}
	static RegistryReader& GetInstance() {
		static RegistryReader registryReader;
		return registryReader;
	}

	bool Open(const std::wstring& strEntry);

	bool Read(const std::wstring& key, DWORD& dwValue);
private:
	HKEY m_hKey;
};

