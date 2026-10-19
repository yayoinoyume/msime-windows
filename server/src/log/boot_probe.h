#pragma once

#include <string>

// Server 启动探针：在 WinMain 里从第一行开始逐阶段落盘。
//
// 为什么单独一套而不是复用 DiagnosticLog：那条路径要读配置（diagnostic_log = false 时整条
// 链路静默）并且要在 InitImeConfig 之后才可用。启动失败恰恰发生在配置和数据目录都还没就绪的
// 时候，所以探针必须自给自足：只依赖 kernel32 的 CreateFileW/WriteFile，路径解析失败就退到
// %TEMP%，再失败就静默放弃，任何情况下都不抛异常、不阻塞启动。
//
// 记录内容只有阶段名、错误码和路径，绝不写用户输入或候选文本。
namespace BootProbe
{
// 本次进程启动的第一个调用。解析并缓存日志路径，写入一行 "begin"。
// pid 与命令行（截断到 200 字符）一并记下：同一台机器上多次启动要能区分开。
void Begin(const char *command_line);

// 追加一个阶段。phase 用简短的英文标识（"config"、"pipe"、"window" 等），detail 可为空。
// 走到哪写到哪，进程崩在下一步时最后一行就是最后完成的那一步。
void Step(const char *phase, const char *detail = nullptr);

// 记录一次失败：阶段名 + Win32 错误码 + 可选补充。失败后调用方通常会 return，
// 所以这一行就是判断「为什么没起来」的直接证据。
void Fail(const char *phase, unsigned long error_code, const char *detail = nullptr);

// 探针日志的完整路径，供其它日志行引用；Begin 之前为空。
const std::wstring &Path();
} // namespace BootProbe
