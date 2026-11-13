#include "win_error.h"

#include <cwchar>

std::wstring Win32ErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring text;
    if (len && buf) {
        text.assign(buf, len);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    }
    if (buf) LocalFree(buf);

    wchar_t hex[16];
    swprintf(hex, 16, L"0x%08lX", static_cast<unsigned long>(code));
    return text.empty() ? std::wstring(L"Error ") + hex : text + L" (" + hex + L")";
}
