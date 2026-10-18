




#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>

namespace
{
    DWORD FindProcess(const wchar_t* exeName)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32W pe = { sizeof(pe) };
        DWORD pid = 0;
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, exeName) == 0)
                {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }


    void EnableDebugPrivilege()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
            return;

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);

        CloseHandle(token);
    }


    DWORD Inject(DWORD pid, const wchar_t* dllPath, HMODULE* module)
    {
        const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

        HANDLE proc = OpenProcess(access, FALSE, pid);
        if (proc == nullptr)
            return GetLastError();

        const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote == nullptr)
        {
            const DWORD err = GetLastError();
            CloseHandle(proc);
            return err;
        }

        DWORD err = 0;
        if (!WriteProcessMemory(proc, remote, dllPath, bytes, nullptr))
        {
            err = GetLastError();
        }
        else
        {

            LPTHREAD_START_ROUTINE loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

            HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
            if (thread == nullptr)
            {
                err = GetLastError();
            }
            else
            {
                if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
                {
                    err = ERROR_TIMEOUT;
                }
                else
                {
                    DWORD code = 0;
                    GetExitCodeThread(thread, &code);
                    *module = reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(code));
                    if (code == 0)
                        err = ERROR_DLL_INIT_FAILED;
                }
                CloseHandle(thread);
            }
        }

        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return err;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 3)
    {
        wprintf(L"usage: %s [process] [dll]\n", argv[0]);
        return 2;
    }


    _setmode(_fileno(stdout), _O_U16TEXT);

    const wchar_t* procName = (argc > 1) ? argv[1] : L"UNDERTALE.exe";

    wchar_t dllPath[MAX_PATH] = {};
    if (argc > 2)
    {
        wcscpy_s(dllPath, argv[2]);
    }
    else
    {
        GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
        wchar_t* slash = wcsrchr(dllPath, L'\\');
        if (slash != nullptr)
            slash[1] = L'\0';
        wcscat_s(dllPath, L"Undertale_Internal.dll");
    }

    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"dll not found: %s\n", dllPath);
        return 1;
    }

    const DWORD pid = FindProcess(procName);
    if (pid == 0)
    {
        wprintf(L"%s is not running\n", procName);
        return 1;
    }

    EnableDebugPrivilege();

    HMODULE module = nullptr;
    const DWORD err = Inject(pid, dllPath, &module);
    if (err != 0)
    {
        wprintf(L"inject failed, error %lu\n", err);
        return 1;
    }

    wprintf(L"injected %s (pid %lu) -> module 0x%08X\n", procName, pid,
            static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(module)));
    return 0;
}
