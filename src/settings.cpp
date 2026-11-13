#include "settings.h"

#include "nv_services.h"

namespace settings {
namespace {

constexpr const wchar_t* kKey = L"Software\\DgpuTraySwitch";

std::optional<DWORD> ReadDword(const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::wstring ReadString(const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2)
        return L"";
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return L"";
    value.resize(wcslen(value.c_str()));
    return value;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

} // namespace

std::optional<bool> DesiredEnabled() {
    auto v = ReadDword(L"DesiredEnabled");
    if (!v) return std::nullopt;
    return *v != 0;
}

void SetDesiredEnabled(bool enabled) {
    DWORD value = enabled ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, L"DesiredEnabled", REG_DWORD, &value, sizeof(value));
}

std::wstring DeviceInstanceId() {
    return Trim(ReadString(L"DeviceInstanceId"));
}

std::vector<std::wstring> Services() {
    std::wstring raw = ReadString(L"Services");
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t next = raw.find(L';', pos);
        if (next == std::wstring::npos) next = raw.size();
        std::wstring item = Trim(raw.substr(pos, next - pos));
        if (!item.empty()) out.push_back(item);
        pos = next + 1;
    }
    return out.empty() ? DefaultNvServices() : out;
}

DWORD NvDelayMs() {
    return ReadDword(L"NvDelayMs").value_or(5000);
}

} // namespace settings
