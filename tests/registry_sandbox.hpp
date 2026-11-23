#pragma once
#include "fixtures.hpp"
#include <windows.h>
#include <string>
class RegistrySandbox final {
    HKEY key_=nullptr;std::wstring path_;
public:
    RegistrySandbox(){path_=L"Software\\WinampOggTests\\"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());DWORD disposition=0;Check(RegCreateKeyExW(HKEY_CURRENT_USER,path_.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key_,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"create isolated test settings");Check(RegOverridePredefKey(HKEY_CURRENT_USER,key_)==ERROR_SUCCESS,"redirect test settings");}
    ~RegistrySandbox(){RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);if(key_)RegCloseKey(key_);RegDeleteTreeW(HKEY_CURRENT_USER,path_.c_str());}
    RegistrySandbox(const RegistrySandbox&)=delete;RegistrySandbox& operator=(const RegistrySandbox&)=delete;
};
