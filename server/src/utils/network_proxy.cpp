#include "utils/network_proxy.h"

#include "config/ime_config.h"

#include <cwctype>

#pragma comment(lib, "winhttp.lib")

namespace NetworkProxy
{
namespace
{

// 本机地址永远直连：自建的翻译/大模型服务常跑在 localhost，让它们绕回代理只会失败。
// 同一份表也是「本机服务免 API 凭据」的判据（见 IsLoopbackEndpoint）。
constexpr const char *kLoopbackHosts[] = {"localhost", "127.0.0.1", "::1"};

// libcurl 要的逗号分隔串，每次请求都会走，拼一次缓存下来。
const std::string &CurlNoProxyList()
{
    static const std::string list = [] {
        std::string built;
        for (const char *host : kLoopbackHosts)
        {
            if (!built.empty())
                built += ',';
            built += host;
        }
        return built;
    }();
    return list;
}

// scheme://[user[:pass]@]host[:port]/path → host，取不出来返回空串。
std::string HostOfEndpoint(const std::string &endpoint)
{
    const std::string scheme = "://";
    const size_t at_scheme = endpoint.find(scheme);
    if (at_scheme == std::string::npos)
        return {};
    std::string rest = endpoint.substr(at_scheme + scheme.size());
    // IPv6 字面量带方括号（http://[::1]:11434/...），方括号里不含 '/'，先按路径切不会切坏。
    const size_t at_path = rest.find('/');
    if (at_path != std::string::npos)
        rest = rest.substr(0, at_path);
    const size_t at_userinfo = rest.rfind('@');
    if (at_userinfo != std::string::npos)
        rest = rest.substr(at_userinfo + 1);
    if (!rest.empty() && rest.front() == '[')
    {
        const size_t close = rest.find(']');
        return close == std::string::npos ? std::string() : rest.substr(1, close - 1);
    }
    const size_t at_port = rest.find(':');
    if (at_port != std::string::npos)
        rest = rest.substr(0, at_port);
    return rest;
}

std::string Narrow(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), bytes, nullptr,
                            nullptr);
    return narrow;
}

std::wstring Widen(const std::string &narrow)
{
    if (narrow.empty())
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
    std::wstring wide(chars > 0 ? static_cast<size_t>(chars) : 0, L'\0');
    if (chars > 0)
        MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), wide.data(), chars);
    return wide;
}

// custom 模式且地址有效时返回 host:port，否则空串（空串 = 走 system 或 none 的逻辑）。
std::string CustomServer(const NetworkProxyConfig &config)
{
    return config.mode == "custom" ? config.server : std::string();
}

// 当前用户的静态系统代理（Internet 选项 → 手动设置代理）。PAC 与自动检测不在这里解析：
// libcurl 没有 PAC 引擎，WinHTTP 一侧由 AUTOMATIC_PROXY 自己处理。
std::string SystemStaticProxy()
{
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&ie))
        return {};
    std::string proxy;
    if (ie.lpszProxy != nullptr)
        proxy = PickHttpsProxyFromWindowsList(ie.lpszProxy);
    if (ie.lpszProxy != nullptr)
        GlobalFree(ie.lpszProxy);
    if (ie.lpszProxyBypass != nullptr)
        GlobalFree(ie.lpszProxyBypass);
    if (ie.lpszAutoConfigUrl != nullptr)
        GlobalFree(ie.lpszAutoConfigUrl);
    return proxy;
}

} // namespace

std::string PickHttpsProxyFromWindowsList(const std::wstring &proxy_list)
{
    std::string plain;
    std::string https;
    std::string http;
    size_t pos = 0;
    while (pos < proxy_list.size())
    {
        size_t end = pos;
        while (end < proxy_list.size() && proxy_list[end] != L';' && !std::iswspace(proxy_list[end]))
            ++end;
        const std::wstring entry = proxy_list.substr(pos, end - pos);
        pos = end + 1;
        if (entry.empty())
            continue;
        const size_t equals = entry.find(L'=');
        if (equals == std::wstring::npos)
        {
            if (plain.empty())
                plain = NormalizeNetworkProxyServer(Narrow(entry));
            continue;
        }
        std::wstring scheme = entry.substr(0, equals);
        for (wchar_t &ch : scheme)
            ch = static_cast<wchar_t>(std::towlower(ch));
        const std::string server = NormalizeNetworkProxyServer(Narrow(entry.substr(equals + 1)));
        if (scheme == L"https" && https.empty())
            https = server;
        else if (scheme == L"http" && http.empty())
            http = server;
    }
    if (!plain.empty())
        return plain;
    return https.empty() ? http : https;
}

// 端点是否跑在本机：本机服务（Ollama 等）不要求 API 凭据。局域网不算——那可能是公司内网
// 或共享机器，是否免鉴权该由用户明示；取不出主机名时按「不是本机」处理。
bool IsLoopbackEndpoint(const std::string &endpoint)
{
    std::string host = HostOfEndpoint(endpoint);
    if (host.empty())
        return false;
    // 主机名大小写不敏感（RFC 3986），用户手填 LOCALHOST 也要认。
    for (char &ch : host)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    for (const char *candidate : kLoopbackHosts)
    {
        if (host == candidate)
            return true;
    }
    // 127.0.0.1 以外的整个 127/8 都指向本机（RFC 1122）。
    return host.rfind("127.", 0) == 0;
}

HINTERNET OpenWinHttpSession(const wchar_t *user_agent)
{
    const NetworkProxyConfig config = GetConfiguredNetworkProxy();
    const std::string custom = CustomServer(config);
    if (!custom.empty())
    {
        const std::wstring server = Widen(custom);
        return WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_NAMED_PROXY, server.c_str(), L"<local>", 0);
    }
    if (config.mode == "none")
        return WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                       0);
}

void ApplyToCurl(CURL *curl)
{
    if (curl == nullptr)
        return;
    const NetworkProxyConfig config = GetConfiguredNetworkProxy();
    std::string server = CustomServer(config);
    if (server.empty() && config.mode == "none")
    {
        // 空串是 libcurl 约定的「显式不用代理」，连 HTTPS_PROXY 等环境变量一并屏蔽。
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        return;
    }
    if (server.empty())
        server = SystemStaticProxy();
    if (server.empty())
        return; // 系统没设静态代理：保持 libcurl 默认（直连，或沿用环境变量）。
    const std::string url = "http://" + server;
    curl_easy_setopt(curl, CURLOPT_PROXY, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROXY, CurlNoProxyList().c_str());
}

} // namespace NetworkProxy
