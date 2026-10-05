#include "RegistryReader.h"
#include <vector>
#include <algorithm>
#include <fstream>
#include <string>
#include <iostream>
RegistryReader::RegistryReader() {

}

bool RegistryReader::Open(const std::wstring& strEntry)
{
    LONG lResult = RegOpenKeyEx(HKEY_CURRENT_USER,
        strEntry.c_str(),
        0, KEY_READ, &m_hKey);

    if (lResult != ERROR_SUCCESS) {
        //std::cerr << "Failed to open registry key. Error code: " << lResult << std::endl;
        return false;;
    }
    return true;

}

bool RegistryReader::Read(const std::wstring& key, DWORD& dwValue)
{
    DWORD dwSize = sizeof(dwValue);
    LONG lResult = RegQueryValueEx(m_hKey, key.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwSize);
    if (lResult == ERROR_SUCCESS) {
        return true;
    }
    else {
        return false;
    }
}

