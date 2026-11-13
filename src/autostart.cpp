#include "autostart.h"

#include "win_error.h"

#include <windows.h>
#include <taskschd.h>
#define SECURITY_WIN32
#include <security.h>

namespace {

constexpr wchar_t kTaskName[] = L"DgpuTraySwitch";

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() { return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

class Bstr {
public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b_; }
private:
    BSTR b_;
};

std::wstring HrText(HRESULT hr) {
    return Win32ErrorText(static_cast<DWORD>(hr));
}

bool ConnectRoot(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& folder, std::wstring& error) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                  reinterpret_cast<void**>(service.put()));
    if (SUCCEEDED(hr)) {
        VARIANT empty = {};
        hr = service->Connect(empty, empty, empty, empty);
    }
    if (SUCCEEDED(hr)) hr = service->GetFolder(Bstr(L"\\"), folder.put());
    if (FAILED(hr)) error = L"Task Scheduler: " + HrText(hr);
    return SUCCEEDED(hr);
}

std::wstring CurrentUser() {
    wchar_t name[256];
    ULONG size = 256;
    if (GetUserNameExW(NameSamCompatible, name, &size)) return name;
    return L"";
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, len);
}

bool Register(ITaskService* service, ITaskFolder* folder, std::wstring& error) {
    VARIANT empty = {};
    ComPtr<ITaskDefinition> task;
    HRESULT hr = service->NewTask(0, task.put());

    const std::wstring user = CurrentUser();
    const std::wstring exe = ExePath();
    const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));

    ComPtr<IRegistrationInfo> info;
    if (SUCCEEDED(hr)) hr = task->get_RegistrationInfo(info.put());
    if (SUCCEEDED(hr)) hr = info->put_Description(Bstr(L"Starts dGPU Tray Switch at logon and restores the dGPU state."));

    // Run in the user's session, elevated, without storing a password.
    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(hr)) hr = task->get_Principal(principal.put());
    if (SUCCEEDED(hr)) hr = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    if (SUCCEEDED(hr)) hr = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    if (SUCCEEDED(hr) && !user.empty()) hr = principal->put_UserId(Bstr(user.c_str()));

    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(hr)) hr = task->get_Settings(settings.put());
    if (SUCCEEDED(hr)) hr = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    if (SUCCEEDED(hr)) hr = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    if (SUCCEEDED(hr)) hr = settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
    if (SUCCEEDED(hr)) hr = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    ComPtr<IIdleSettings> idle;
    if (SUCCEEDED(hr)) hr = settings->get_IdleSettings(idle.put());
    if (SUCCEEDED(hr)) hr = idle->put_StopOnIdleEnd(VARIANT_FALSE);

    // Fire at this user's logon only.
    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logon;
    if (SUCCEEDED(hr)) hr = task->get_Triggers(triggers.put());
    if (SUCCEEDED(hr)) hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
    if (SUCCEEDED(hr)) hr = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.put()));
    if (SUCCEEDED(hr) && !user.empty()) hr = logon->put_UserId(Bstr(user.c_str()));

    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    if (SUCCEEDED(hr)) hr = task->get_Actions(actions.put());
    if (SUCCEEDED(hr)) hr = actions->Create(TASK_ACTION_EXEC, action.put());
    if (SUCCEEDED(hr)) hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.put()));
    if (SUCCEEDED(hr)) hr = exec->put_Path(Bstr(exe.c_str()));
    if (SUCCEEDED(hr)) hr = exec->put_Arguments(Bstr(L"--boot"));
    if (SUCCEEDED(hr)) hr = exec->put_WorkingDirectory(Bstr(dir.c_str()));

    ComPtr<IRegisteredTask> registered;
    if (SUCCEEDED(hr)) {
        hr = folder->RegisterTaskDefinition(Bstr(kTaskName), task.get(), TASK_CREATE_OR_UPDATE, empty, empty,
                                            TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.put());
    }
    if (FAILED(hr)) error = L"Cannot register logon task: " + HrText(hr);
    return SUCCEEDED(hr);
}

} // namespace

bool IsAutostartEnabled() {
    std::wstring error;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    if (!ConnectRoot(service, folder, error)) return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(folder->GetTask(Bstr(kTaskName), task.put()));
}

bool SetAutostart(bool enable, std::wstring& error) {
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    if (!ConnectRoot(service, folder, error)) return false;
    if (enable) return Register(service.get(), folder.get(), error);

    HRESULT hr = folder->DeleteTask(Bstr(kTaskName), 0);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        error = L"Cannot remove logon task: " + HrText(hr);
        return false;
    }
    return true;
}
