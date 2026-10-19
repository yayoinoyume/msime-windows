#include "log/boot_probe.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

#include "utils/common_utils.h"

namespace
{
std::mutex g_mutex;
std::wstring g_path;
bool g_resolved = false;
// 探针自身失败时不再反复尝试，免得每个 Step 都去撞同一堵墙。
bool g_disabled = false;

// 依次尝试：数据目录 logs\、%LOCALAPPDATA%\metasequoiaime\logs\、%TEMP%。
// 用 create 而不是只算路径：目录不存在时直接建出来，写不进去就换下一个候选。
bool TryOpenIn(const std::wstring &directory, std::wstring &out_path)
{
    if (directory.empty())
        return false;

    // 逐级建目录：数据目录本身可能还没被安装器创建。
    std::wstring current;
    for (size_t i = 0; i < directory.size(); ++i)
    {
        current.push_back(directory[i]);
        const bool at_separator = directory[i] == L'\\' || directory[i] == L'/';
        if (!at_separator)
            continue;
        if (current.size() > 1 && current[current.size() - 2] == L':')
            continue; // 卷标 "C:\" 已存在，CreateDirectory 会失败但无害
        CreateDirectoryW(current.c_str(), nullptr);
    }
    CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring candidate = directory + L"\\server-boot.log";
    HANDLE file = CreateFileW(candidate.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(file);
    out_path = candidate;
    return true;
}

const std::wstring &ResolvePathUnlocked()
{
    if (g_resolved)
        return g_path;
    g_resolved = true;

    // 1) 输入法数据目录（安装后就是用户选的位置）。
    const std::wstring data_path = CommonUtils::get_ime_data_path_w();
    if (!data_path.empty() && TryOpenIn(data_path + L"\\logs", g_path))
        return g_path;

    // 2) LocalAppData 下的默认位置：数据目录解析失败时仍然有地方落盘。
    const std::wstring local_appdata = CommonUtils::get_local_appdata_path_w();
    if (!local_appdata.empty() && TryOpenIn(local_appdata + L"\\metasequoiaime\\logs", g_path))
        return g_path;

    // 3) 临时目录：前两个都不可写时的最后兜底（例如权限受限的受信任目录）。
    wchar_t temp[MAX_PATH]{};
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
    if (temp_length > 0 && temp_length < MAX_PATH)
    {
        std::wstring temp_directory(temp);
        while (!temp_directory.empty() && (temp_directory.back() == L'\\' || temp_directory.back() == L'/'))
            temp_directory.pop_back();
        if (TryOpenIn(temp_directory, g_path))
            return g_path;
    }

    g_disabled = true;
    return g_path;
}

void AppendUnlocked(const std::wstring &line)
{
    const std::wstring &path = ResolvePathUnlocked();
    if (path.empty() || g_disabled)
        return;

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    // UTF-8 落盘，Windows 记事本与 VS Code 都能直接打开；行尾用 CRLF。
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0)
    {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr,
                            nullptr);
        utf8 += "\r\n";
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

std::wstring Timestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds));
    return buffer;
}

std::wstring Widen(const char *text)
{
    if (text == nullptr || *text == '\0')
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1)
        return {};
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    return wide;
}

void Write(const wchar_t *kind, const std::wstring &body)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::wstring line = Timestamp();
    line += L" [";
    line += kind;
    line += L"] pid=";
    line += std::to_wstring(GetCurrentProcessId());
    line += L" ";
    line += body;
    AppendUnlocked(line);
}
} // namespace

namespace BootProbe
{
void Begin(const char *command_line)
{
    std::wstring detail = L"start";
    if (command_line != nullptr && *command_line != '\0')
    {
        std::wstring command = Widen(command_line);
        // 命令行可能很长，也只用于区分启动来源，截断即可。
        constexpr size_t kMaxCommandChars = 200;
        if (command.size() > kMaxCommandChars)
        {
            command.resize(kMaxCommandChars);
            command += L"...";
        }
        detail += L" cmd=";
        detail += command;
    }
    Write(L"begin", detail);
}

void Step(const char *phase, const char *detail)
{
    std::wstring body = Widen(phase);
    if (detail != nullptr && *detail != '\0')
    {
        body += L" ";
        body += Widen(detail);
    }
    Write(L"step", body);
}

void Fail(const char *phase, unsigned long error_code, const char *detail)
{
    std::wstring body = Widen(phase);
    body += L" FAILED err=";
    body += std::to_wstring(error_code);
    if (detail != nullptr && *detail != '\0')
    {
        body += L" ";
        body += Widen(detail);
    }
    Write(L"fail", body);
}

const std::wstring &Path()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return ResolvePathUnlocked();
}
} // namespace BootProbe
