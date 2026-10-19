// 配置同步的网络与落盘部分：WebDAV 读写、三方合并落地、冲突留证、后台定时器。
#include "sync/config_sync.h"

#include "config/ime_config.h"
#include "config/ime_config_internal.h"
#include "utils/network_proxy.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace ConfigSync
{
namespace
{
constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 20000;
// 同步包只有几百行，1 MiB 足够，也挡住远端被替换成大文件。
constexpr size_t kMaxResponseBytes = 1024 * 1024;

std::mutex g_mutex;                                    // 串行化一次完整同步
std::mutex g_conflict_mutex;                           // 保护 g_pending_conflict
std::map<std::string, std::string> g_pending_conflict; // 本轮冲突的远端值，供设置页选择
std::atomic<bool> g_stop{false};
std::thread g_scheduler;
std::condition_variable g_wake;
std::mutex g_wake_mutex;
int g_wake_generation = 0;
// 调度线程已处理过的代数，用来分辨「被唤醒」和「自然到点」。
int g_seen_generation = 0;

struct Response
{
    CURLcode code = CURLE_FAILED_INIT;
    long status = 0;
    std::string body;
    std::string error;
    bool overflow = false;
};

void InitCurl()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    const size_t bytes = size * count;
    auto *response = static_cast<Response *>(user);
    if (bytes > kMaxResponseBytes - (std::min)(response->body.size(), kMaxResponseBytes))
    {
        response->overflow = true;
        return 0;
    }
    response->body.append(data, bytes);
    return bytes;
}

// 用户在设置里填的是目录（例如 https://dav.example.com/msime），拼上文件名才是目标。
// 没填文件名的一律按目录处理，允许带或不带结尾斜杠。
std::string RemoteUrl(const Settings &settings)
{
    return RemoteTargetForTests(settings.url);
}

Response Perform(const std::string &method, const std::string &url, const Settings &settings,
                 const std::string *payload)
{
    InitCurl();
    Response response;
    CURL *curl = curl_easy_init();
    if (!curl)
        return response;
    char error[CURL_ERROR_SIZE] = {};
    curl_slist *headers = nullptr;
    if (!settings.user.empty() || !settings.password.empty())
    {
        // Basic 认证。密码在 config.toml 里是 DPAPI 密文，取出来时已经解密，不出现在日志里。
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERNAME, settings.user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, settings.password.c_str());
    }
    if (!payload)
        headers = curl_slist_append(headers, "Accept: */*");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (payload)
    {
        headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload->size()));
    }
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    response.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.error = error;
    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// GET 远端快照。远端文件还不存在是正常情况（第一次同步），返回空表并置 exists=false。
bool FetchRemote(const Settings &settings, std::map<std::string, std::string> &snapshot, bool &exists,
                 std::string &message)
{
    exists = false;
    snapshot.clear();
    const std::string url = RemoteUrl(settings);
    if (url.empty())
    {
        message = "还没填 WebDAV 地址";
        return false;
    }
    const Response response = Perform("GET", url, settings, nullptr);
    if (response.code != CURLE_OK)
    {
        message = "连不上 WebDAV：" + (response.error.empty() ? curl_easy_strerror(response.code) : response.error);
        return false;
    }
    if (response.status == 404)
        return true;
    if (response.status == 401 || response.status == 403)
    {
        message = "WebDAV 拒绝访问，检查用户名和密码";
        return false;
    }
    if (response.status < 200 || response.status >= 300)
    {
        message = "读取云端配置失败：HTTP " + std::to_string(response.status);
        return false;
    }
    if (response.overflow)
    {
        message = "云端配置超过 1 MiB，不像是本输入法的同步文件";
        return false;
    }
    if (!ParseSnapshot(response.body, snapshot))
    {
        message = "云端文件格式不对，可能是别的程序的同名文件";
        return false;
    }
    exists = true;
    return true;
}

bool PushRemote(const Settings &settings, const std::map<std::string, std::string> &snapshot, std::string &message)
{
    const std::string url = RemoteUrl(settings);
    if (url.empty())
    {
        message = "还没填 WebDAV 地址";
        return false;
    }
    const std::string payload = SerializeSnapshot(snapshot);
    const Response response = Perform("PUT", url, settings, &payload);
    if (response.code != CURLE_OK)
    {
        message = "上传失败：" + (response.error.empty() ? curl_easy_strerror(response.code) : response.error);
        return false;
    }
    if (response.status == 401 || response.status == 403)
    {
        message = "WebDAV 拒绝访问，检查用户名和密码";
        return false;
    }
    // PUT 到不存在的父目录时，服务器返回 404 或 409，提示比裸状态码更有用。
    if (response.status == 404 || response.status == 409)
    {
        message = "云端目录不存在，先在 WebDAV 里建好这个目录";
        return false;
    }
    if (response.status < 200 || response.status >= 300)
    {
        message = "上传失败：HTTP " + std::to_string(response.status);
        return false;
    }
    return true;
}

// 写基线：只有落盘成功，下次才认为「这些值两台机器都见过」。
bool WriteBaseline(const std::map<std::string, std::string> &snapshot)
{
    return ime_config_detail::WriteFileTextAtomically(GetImeConfigPath().parent_path() / "sync_state.txt",
                                                      SerializeSnapshot(snapshot));
}

// 把要跟进的键写进 config.toml。走现有的写入口，凭证加密、格式保留和 Server 重载通知都由它负责；
// 一个键失败就整体失败，避免本地和基线各记一半。
bool ApplyLocal(const std::map<std::string, std::string> &values)
{
    if (values.empty())
        return true;
    std::vector<ime_config_detail::ConfigValueUpdate> updates;
    updates.reserve(values.size());
    for (const auto &[assignment_id, literal] : values)
    {
        const size_t separator = assignment_id.find('\x01');
        if (separator == std::string::npos)
            continue;
        updates.push_back({assignment_id.substr(0, separator), assignment_id.substr(separator + 1), literal});
    }
    return ime_config_detail::WriteConfiguredValues(updates);
}

// 冲突的两份都留在数据目录里，文件名带时间戳，重复同步不会互相覆盖。
void SaveConflictCopies(const std::map<std::string, std::string> &local,
                        const std::map<std::string, std::string> &remote, const std::vector<std::string> &keys)
{
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::map<std::string, std::string> local_snapshot;
    std::map<std::string, std::string> remote_snapshot;
    for (const auto &key : keys)
    {
        if (const auto found = local.find(key); found != local.end())
            local_snapshot[key] = found->second;
        if (const auto found = remote.find(key); found != remote.end())
            remote_snapshot[key] = found->second;
    }
    const auto directory = GetImeConfigPath().parent_path();
    (void)ime_config_detail::WriteFileTextAtomically(
        directory / ("sync_conflict_local_" + std::to_string(stamp) + ".txt"), SerializeSnapshot(local_snapshot));
    (void)ime_config_detail::WriteFileTextAtomically(
        directory / ("sync_conflict_remote_" + std::to_string(stamp) + ".txt"), SerializeSnapshot(remote_snapshot));
    std::lock_guard<std::mutex> lock(g_conflict_mutex);
    g_pending_conflict.clear();
    for (const auto &key : keys)
    {
        const auto found = remote.find(key);
        if (found != remote.end())
            g_pending_conflict[key] = found->second;
    }
}

// 冲突解决后清掉待决状态，免得设置页一直提示。
void ClearPendingConflict()
{
    std::lock_guard<std::mutex> lock(g_conflict_mutex);
    g_pending_conflict.clear();
}

int IntervalSeconds(const std::string &interval)
{
    if (interval == "5m")
        return 5 * 60;
    if (interval == "15m")
        return 15 * 60;
    if (interval == "30m")
        return 30 * 60;
    if (interval == "60m")
        return 60 * 60;
    return 0; // manual
}
} // namespace

Settings GetSettings()
{
    // 一次取整份快照：四个字段分别读会被一次并发的设置写入拆开，拿到自相矛盾的组合。
    const ConfigSyncConfig config = GetConfiguredConfigSync();
    Settings settings;
    settings.enabled = config.enabled;
    settings.url = config.url;
    settings.user = config.user;
    settings.password = config.password;
    settings.interval = config.interval;
    if (settings.interval != "manual" && settings.interval != "5m" && settings.interval != "15m" &&
        settings.interval != "30m" && settings.interval != "60m")
        settings.interval = "15m";
    return settings;
}

bool SetSettingsString(const std::string &key, const std::string &value)
{
    return SetConfiguredSyncString(key, value);
}

bool SetSettingsBool(const std::string &key, bool value)
{
    return SetConfiguredSyncBool(key, value);
}

Result Run(Direction direction)
{
    // 手动按钮和定时器可能同时触发，串行化；第二个进来时直接告诉用户上一次还没完。
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return {Status::Failed, "上一次同步还没结束", 0};

    const Settings settings = GetSettings();
    if (settings.url.empty())
        return {Status::Disabled, "还没填 WebDAV 地址", 0};
    // 后台定时器走 Auto：开关关掉或选了「仅手动」就不该自己动，也不能顺手把本机推上去。
    if (direction == Direction::Auto && (!settings.enabled || settings.interval == "manual"))
        return {Status::Disabled, "自动同步未开启", 0};

    std::map<std::string, std::string> remote;
    bool remote_exists = false;
    std::string message;
    if (!FetchRemote(settings, remote, remote_exists, message))
        return {Status::Failed, message, 0};

    const auto local = ReadLocalSnapshot();
    // 只上传时基线没有意义：用户明确要求把本机推上去。
    if (direction == Direction::Upload)
    {
        if (!PushRemote(settings, local, message))
            return {Status::Failed, message, 0};
        (void)WriteBaseline(local);
        ClearPendingConflict();
        return {Status::Uploaded, "已上传 " + std::to_string(local.size()) + " 项设置", static_cast<int>(local.size())};
    }

    // 远端还没有文件。手动「立即同步」时把本机当唯一来源上传；「拉取」则没什么可拉的。
    if (!remote_exists)
    {
        if (direction == Direction::Download)
            return {Status::UpToDate, "云端还没有同步文件，先在本机点「上传本机设置」", 0};
        if (!PushRemote(settings, local, message))
            return {Status::Failed, message, 0};
        (void)WriteBaseline(local);
        ClearPendingConflict();
        return {Status::Uploaded, "首次同步：已上传 " + std::to_string(local.size()) + " 项设置",
                static_cast<int>(local.size())};
    }

    const auto baseline = ReadBaseline();
    const Plan plan = ComputeMerge(local, baseline, remote);
    if (!plan.conflicts.empty())
    {
        SaveConflictCopies(local, remote, plan.conflicts);
        return {Status::Conflict, std::to_string(plan.conflicts.size()) + " 项设置在本机和云端都改过",
                static_cast<int>(plan.conflicts.size())};
    }

    // 单向拉取：只应用云端较新的键，不把本机较新的键推上去。
    if (direction == Direction::Download)
    {
        if (plan.apply.empty())
            return {Status::UpToDate, "云端没有需要合并的改动", 0};
        if (!ApplyLocal(plan.apply))
            return {Status::Failed, "写入本地 config.toml 失败", 0};
        std::map<std::string, std::string> pulled = local;
        for (const auto &[key, literal] : plan.apply)
            pulled[key] = literal;
        (void)WriteBaseline(pulled);
        ClearPendingConflict();
        return {Status::Pulled, "已拉取 " + std::to_string(plan.apply.size()) + " 项设置",
                static_cast<int>(plan.apply.size())};
    }

    if (plan.apply.empty() && plan.push.empty())
    {
        (void)WriteBaseline(local);
        ClearPendingConflict();
        return {Status::UpToDate, "已是最新", 0};
    }

    // 先写本地再推远端：本地写失败时远端还没动，用户重试就是了；反过来会留下一个
    // 「云端已经是新版、本机还是旧版、基线也更新了」的死角。
    if (!ApplyLocal(plan.apply))
        return {Status::Failed, "写入本地 config.toml 失败", 0};

    std::map<std::string, std::string> merged = local;
    for (const auto &[key, literal] : plan.apply)
        merged[key] = literal;
    for (const auto &[key, literal] : plan.push)
        merged[key] = literal;
    if (!PushRemote(settings, merged, message))
    {
        // 本地已经跟上了，基线先不动：下次会重算一遍，把还差的那部分再推一次。
        return {Status::Failed, message, 0};
    }
    (void)WriteBaseline(merged);
    ClearPendingConflict();

    const int applied = static_cast<int>(plan.apply.size());
    const int pushed = static_cast<int>(plan.push.size());
    if (!plan.apply.empty() && !plan.push.empty())
        return {Status::Merged,
                "合并完成：应用 " + std::to_string(applied) + " 项，上传 " + std::to_string(pushed) + " 项",
                applied + pushed};
    if (!plan.apply.empty())
        return {Status::Pulled, "已拉取 " + std::to_string(applied) + " 项设置", applied};
    return {Status::Uploaded, "已上传 " + std::to_string(pushed) + " 项设置", pushed};
}

Result ResolveConflict(bool keep_local)
{
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return {Status::Failed, "上一次同步还没结束", 0};

    std::map<std::string, std::string> pending;
    {
        std::lock_guard<std::mutex> conflict_lock(g_conflict_mutex);
        pending = g_pending_conflict;
    }
    if (pending.empty())
        return {Status::Failed, "没有待解决的冲突", 0};

    const Settings settings = GetSettings();
    if (settings.url.empty())
        return {Status::Disabled, "还没填 WebDAV 地址", 0};
    // 后台定时器走 Auto：开关关掉或选了「仅手动」就不该自己动，也不能顺手把本机推上去。
    if (direction == Direction::Auto && (!settings.enabled || settings.interval == "manual"))
        return {Status::Disabled, "自动同步未开启", 0};

    std::map<std::string, std::string> remote;
    bool remote_exists = false;
    std::string message;
    if (!FetchRemote(settings, remote, remote_exists, message))
        return {Status::Failed, message, 0};

    std::map<std::string, std::string> local = ReadLocalSnapshot();
    const auto baseline = ReadBaseline();
    if (keep_local)
    {
        // 本机版本推到云端，基线跟着记成本机的值。
        std::map<std::string, std::string> merged = remote_exists ? remote : local;
        for (const auto &[key, literal] : local)
            merged[key] = literal;
        if (!PushRemote(settings, merged, message))
            return {Status::Failed, message, 0};
        (void)WriteBaseline(local);
        ClearPendingConflict();
        return {Status::Uploaded, "已用本机设置覆盖云端", static_cast<int>(pending.size())};
    }

    if (!ApplyLocal(pending))
        return {Status::Failed, "写入本地 config.toml 失败", 0};
    std::map<std::string, std::string> merged = baseline;
    for (const auto &[key, literal] : remote)
        merged[key] = literal;
    (void)WriteBaseline(merged);
    ClearPendingConflict();
    return {Status::Pulled, "已用云端设置覆盖本机", static_cast<int>(pending.size())};
}

void StartScheduler()
{
    StopScheduler();
    g_stop = false;
    {
        std::lock_guard<std::mutex> lock(g_wake_mutex);
        g_seen_generation = g_wake_generation;
    }
    // 常驻 Server 里跑：设置窗口关掉后同步还得继续。等待以 60 秒为上限分片，这样
    // 改了间隔（或刚关掉同步）最多一分钟就按新配置重排，不会等满一个旧间隔。
    g_scheduler = std::thread([] {
        constexpr int kSliceSeconds = 60;
        int elapsed = 0;
        while (!g_stop)
        {
            {
                std::unique_lock<std::mutex> lock(g_wake_mutex);
                g_wake.wait_for(lock, std::chrono::seconds(kSliceSeconds),
                                [] { return g_stop.load() || g_wake_generation != g_seen_generation; });
            }
            if (g_stop)
                break;
            if (g_wake_generation != g_seen_generation)
            {
                // 配置变了：重新按新间隔计时。
                g_seen_generation = g_wake_generation;
                elapsed = 0;
                continue;
            }
            const Settings settings = GetSettings();
            const int seconds = settings.enabled ? IntervalSeconds(settings.interval) : 0;
            if (seconds <= 0)
            {
                elapsed = 0;
                continue;
            }
            elapsed += kSliceSeconds;
            if (elapsed < seconds)
                continue;
            elapsed = 0;
            (void)Run(Direction::Auto);
        }
    });
}

void StopScheduler()
{
    g_stop = true;
    {
        std::lock_guard<std::mutex> lock(g_wake_mutex);
        ++g_wake_generation;
    }
    g_wake.notify_all();
    if (g_scheduler.joinable())
        g_scheduler.join();
}

void NotifySettingsChanged()
{
    {
        std::lock_guard<std::mutex> lock(g_wake_mutex);
        ++g_wake_generation;
    }
    g_wake.notify_all();
}

std::map<std::string, std::string> PendingConflict()
{
    std::lock_guard<std::mutex> lock(g_conflict_mutex);
    return g_pending_conflict;
}
} // namespace ConfigSync
