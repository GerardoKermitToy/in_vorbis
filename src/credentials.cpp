// SPDX-License-Identifier: GPL-3.0-only
#include "credentials.hpp"
#include <wincred.h>
#include <objbase.h>
#include <array>
#include <vector>
namespace ogg_input {
bool AskCredentials(HWND parent,bool proxy,const std::wstring& server,std::wstring& user,std::wstring& password){
    CREDUI_INFOW prompt{};prompt.cbSize=sizeof(prompt);prompt.hwndParent=parent;
    prompt.pszCaptionText=proxy?L"Ogg Proxy Authentication":L"Ogg Stream Authentication";
    std::wstring text=L"Credentials requested by "+server+L". They are used only for this connection and are not saved.";
    prompt.pszMessageText=text.c_str();ULONG package=0;void* packed=nullptr;ULONG size=0;BOOL save=FALSE;
    auto status=CredUIPromptForWindowsCredentialsW(&prompt,0,&package,nullptr,0,&packed,&size,&save,CREDUIWIN_GENERIC);
    if(status!=ERROR_SUCCESS)return false;
    std::array<wchar_t,513> name{},domain{},secret{};DWORD names=static_cast<DWORD>(name.size()),domains=static_cast<DWORD>(domain.size()),secrets=static_cast<DWORD>(secret.size());
    BOOL ok=CredUnPackAuthenticationBufferW(0,packed,size,name.data(),&names,domain.data(),&domains,secret.data(),&secrets);
    SecureZeroMemory(packed,size);CoTaskMemFree(packed);
    if(ok){user=domain[0]?std::wstring(domain.data())+L"\\"+name.data():name.data();password=secret.data();}
    SecureZeroMemory(secret.data(),sizeof(secret));return ok!=FALSE;
}
}
