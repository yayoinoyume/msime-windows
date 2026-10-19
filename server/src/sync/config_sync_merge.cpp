// 配置同步的协议与合并规则。网络部分在 config_sync_webdav.cpp，这里只做纯计算，便于单测。
#include "sync/config_sync.h"

#include "config/ime_config.h"
#include "config/ime_config_internal.h"

#include <algorithm>
#include <set>

namespace ConfigSync
{
namespace
{
// 远端文件头。格式变了就换版本号，老客户端会拒绝而不是误读。
constexpr const char *kHeader = "# MetasequoiaIME 配置同步 v1";

// 参与同步的段。凭证集中在 [ai_assistant] / [voice_input] / [tencent_tmt] /
// [niutrans] / [custom_translation] 这些段里，即使逐个键再过滤一层，也不把整段列进来：
// 少一个段就等于少一类漏出去的路径。
const std::set<std::string> &SyncableSections()
{
    static const std::set<std::string> sections = {
        "general", "appearance",  "language_bar",        "input", "helpcode",
        "quanpin", "keybindings", "dictionary",          "skin",  "soft_keyboard",
        "utility", "statistics",  "frequency_adjustment"};
    return sections;
}

// [sync] 段自己永远不同步：把 WebDAV 密码同步到别的机器既没必要，也会让两台机器互相改写地址。
std::string SectionOf(const std::string &assignment_id)
{
    const size_t separator = assignment_id.find('\x01');
    return separator == std::string::npos ? std::string() : assignment_id.substr(0, separator);
}

std::string KeyOf(const std::string &assignment_id)
{
    const size_t separator = assignment_id.find('\x01');
    return separator == std::string::npos ? std::string() : assignment_id.substr(separator + 1);
}

std::string ToHex(const std::string &text)
{
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(text.size() * 2);
    for (const char ch : text)
    {
        const auto byte = static_cast<unsigned char>(ch);
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    return hex;
}

bool FromHex(const std::string &hex, std::string &text)
{
    if (hex.size() % 2 != 0)
        return false;
    text.clear();
    text.reserve(hex.size() / 2);
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index < hex.size(); index += 2)
    {
        const int high = digit(hex[index]);
        const int low = digit(hex[index + 1]);
        if (high < 0 || low < 0)
            return false;
        text.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::string Trim(const std::string &text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::filesystem::path StatePath()
{
    return GetImeConfigPath().parent_path() / "sync_state.txt";
}
} // namespace

std::string RemoteTargetForTests(const std::string &configured_url)
{
    // 结尾斜杠可有可无；用户已经填了文件名就不要再拼一次。
    std::string url = configured_url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    if (url.empty())
        return {};
    const std::string name = kRemoteFileName;
    if (url.size() >= name.size() && url.compare(url.size() - name.size(), std::string::npos, name) == 0)
        return url;
    return url + "/" + name;
}

bool IsSyncableKey(const std::string &assignment_id)
{
    const std::string section = SectionOf(assignment_id);
    if (SyncableSections().count(section) == 0)
        return false;
    // 段已经筛过一遍，这里再挡一层：任何看起来像凭证的键名都不入包。
    return !ime_config_detail::IsCredentialKey(KeyOf(assignment_id));
}

std::map<std::string, std::string> ReadLocalSnapshot()
{
    std::map<std::string, std::string> snapshot;
    const auto assignments =
        ime_config_detail::ParseTomlAssignments(ime_config_detail::ReadFileText(GetImeConfigPath()));
    for (const auto &[assignment_id, literal] : assignments)
    {
        if (IsSyncableKey(assignment_id))
            snapshot[assignment_id] = literal;
    }
    return snapshot;
}

std::map<std::string, std::string> ReadBaseline()
{
    std::map<std::string, std::string> baseline;
    ParseSnapshot(ime_config_detail::ReadFileText(StatePath()), baseline);
    return baseline;
}

std::string SerializeSnapshot(const std::map<std::string, std::string> &snapshot)
{
    std::string text = std::string(kHeader) + "\n";
    for (const auto &[assignment_id, literal] : snapshot)
        text += ToHex(assignment_id) + " = " + literal + "\n";
    return text;
}

bool ParseSnapshot(const std::string &text, std::map<std::string, std::string> &snapshot)
{
    snapshot.clear();
    size_t begin = 0;
    while (begin <= text.size())
    {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = Trim(text.substr(begin, end - begin));
        begin = end + 1;
        if (line.empty() || line[0] == '#')
            continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            return false;
        std::string assignment_id;
        if (!FromHex(Trim(line.substr(0, equals)), assignment_id))
            return false;
        snapshot[assignment_id] = Trim(line.substr(equals + 1));
    }
    return true;
}

Plan ComputeMerge(const std::map<std::string, std::string> &local, const std::map<std::string, std::string> &baseline,
                  const std::map<std::string, std::string> &remote)
{
    Plan plan;
    const auto value_at = [](const std::map<std::string, std::string> &map, const std::string &key) {
        const auto found = map.find(key);
        return found == map.end() ? std::string() : found->second;
    };
    std::set<std::string> keys;
    for (const auto &[key, unused] : local)
        keys.insert(key);
    for (const auto &[key, unused] : remote)
        keys.insert(key);

    for (const auto &key : keys)
    {
        const auto found_local = local.find(key);
        const auto found_remote = remote.find(key);
        const std::string base = value_at(baseline, key);
        const std::string mine = found_local == local.end() ? std::string() : found_local->second;
        const std::string theirs = found_remote == remote.end() ? std::string() : found_remote->second;
        const bool local_changed = (found_local != local.end()) && mine != base;
        const bool remote_changed = (found_remote != remote.end()) && theirs != base;
        if (!local_changed && !remote_changed)
            continue;
        if (local_changed && !remote_changed)
        {
            plan.push[key] = mine;
            continue;
        }
        if (!local_changed && remote_changed)
        {
            plan.apply[key] = theirs;
            continue;
        }
        if (mine == theirs)
            continue;
        plan.conflicts.push_back(key);
    }
    return plan;
}
} // namespace ConfigSync
