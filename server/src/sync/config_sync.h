#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// 配置同步：把 config.toml 里「用户改过的键」搬到 WebDAV 上的一个小文件，别的机器再拉回来。
//
// 同步的单位是键而不是文件。整份 config.toml 不能直接覆盖：里面既有本机安装的出厂模板内容，
// 也有 DPAPI 加密后绑定本机本账户的凭证（换机器解不开，读回时一律当没填），直接覆盖会静默
// 丢密钥、也会把别人的出厂默认值带进来。所以这里只搬 [general] / [appearance] 这类普通键，
// 凭证键（IsCredentialKey）永不入包，而 config.base.toml、词库、models\ 等根本不在候选范围内。
//
// 远端格式是一行一个键的文本文件，只要几百字节，人可以读也可以 diff：
//     # MetasequoiaIME 配置同步 v1
//     general\001diagnostic_log = true
//     appearance\001theme_mode = "dark"
// 键用 TOML 赋值 id（段名 \x01 键名）的 UTF-16 十六进制形式存，值就是 config.toml 里的原始字面量，
// 因此合并时不需要重新序列化，格式与注释也都留在各自的本地文件里。
//
// 合并是三方的：本地当前值、上次同步时的本地值（基线）、远端值。只有真冲突——两边都改过且改得不一样——
// 才回来问用户，两个副本都留着，绝不静默覆盖。
namespace ConfigSync
{
// WebDAV 上的目标文件，放在用户在设置里填的目录下。
constexpr const char *kRemoteFileName = "metasequoiaime-config.toml";

// [sync] 段。地址、用户名、密码存 config.toml，密码走现有的 DPAPI 加密。
struct Settings
{
    bool enabled = false;
    std::string url;
    std::string user;
    std::string password;
    // manual | 5m | 15m | 30m | 60m
    std::string interval = "15m";
};

enum class Status
{
    Disabled, // 没开同步，或地址没填
    UpToDate, // 本地、基线、远端三方一致
    Uploaded, // 远端为空或已被本机领先，只上传
    Pulled,   // 只拉取并应用
    Merged,   // 两边各改了一部分不重叠的键，合并后上传
    Conflict, // 同一个键两边都改过且不同：没写任何文件，冲突留给用户
    Failed,   // 网络、目录、格式等错误，见 message
};

struct Result
{
    Status status = Status::Disabled;
    // 给人看的一句话，冲突和失败时用。
    std::string message;
    // 实际动作涉及的键数，给设置页显示「已同步 N 项」。
    int changed_keys = 0;
};

// 读取 [sync] 段。每次现读，设置页改完立刻生效。
Settings GetSettings();

// 计算本地快照与基线、远端的差异并把结果落到 report。不做任何网络请求，供测试和设置页
// 「本地与云端对比」使用。
struct Plan
{
    // 远端有、本地要跟着改的键（TOML 赋值 id -> 新字面量）。
    std::map<std::string, std::string> apply;
    // 本地要推给远端的键。
    std::map<std::string, std::string> push;
    // 两边都改过且不同的键。
    std::vector<std::string> conflicts;
};
Plan ComputeMerge(const std::map<std::string, std::string> &local, const std::map<std::string, std::string> &baseline,
                  const std::map<std::string, std::string> &remote);

// 本地快照：config.toml 里所有可同步的键。凭证键已剔除。
std::map<std::string, std::string> ReadLocalSnapshot();
// 基线快照：上次同步时的本地值，落在数据目录的 sync_state.txt。
std::map<std::string, std::string> ReadBaseline();
// 用户在设置里填的目录 + 固定文件名。填的已经是文件名时原样返回，结尾斜杠可有可无。
// 暴露成公开入口只为让这条拼装规则能被单测覆盖。
std::string RemoteTargetForTests(const std::string &configured_url);
// 一个键（TSF 赋值 id）是否参与同步。
bool IsSyncableKey(const std::string &assignment_id);
// 远端文件文本 <-> 键值表。
std::string SerializeSnapshot(const std::map<std::string, std::string> &snapshot);
bool ParseSnapshot(const std::string &text, std::map<std::string, std::string> &snapshot);

// 一次完整同步：拉远端、三方合并、写本地、推远端、更新基线。
// 设置页的两个按钮和后台定时器都调这一个入口；`direction` 限定只上传或只下载。
enum class Direction
{
    Auto,   // 双向合并（默认）
    Upload, // 只把本地推上去
    Download,
};
Result Run(Direction direction);
// 应用冲突的某一侧：`keep_local` 为真就把本地版本推上去，否则用远端版本覆盖本地。
Result ResolveConflict(bool keep_local);

// 后台定时器：按 Settings 的 interval 起停。设置页改完 [sync] 段后调 NotifySettingsChanged
// 让它立刻按新间隔重排，否则要等满一个旧间隔才生效。
void StartScheduler();
void StopScheduler();
void NotifySettingsChanged();

// 待解决的冲突（键 -> 云端值）。设置页据此显示「本机 / 云端」二选一。
std::map<std::string, std::string> PendingConflict();
} // namespace ConfigSync
