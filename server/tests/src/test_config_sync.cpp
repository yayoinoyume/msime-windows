// 配置同步的纯逻辑：参与范围（哪些键能同步）、远端文件格式、三方合并与冲突判定。
// 网络部分不在这里测——那需要一个 WebDAV 服务端，留给手动验收。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "sync/config_sync.h"
#include "tests/includes/test_framework.h"

#include <string>

namespace
{
// 段名 + \x01 + 键名，与 config.toml 里的 TOML 赋值 id 一致。
std::string Id(const char *section, const char *key)
{
    return std::string(section) + '\x01' + key;
}
} // namespace

TEST_CASE(config_sync_scope_keeps_settings_but_drops_credentials)
{
    // 普通设置键参与同步。
    REQUIRE(ConfigSync::IsSyncableKey(Id("general", "diagnostic_log")));
    REQUIRE(ConfigSync::IsSyncableKey(Id("appearance", "theme_mode")));
    REQUIRE(ConfigSync::IsSyncableKey(Id("input", "schema")));

    // 凭证键一律不入包，即使段本身是可同步的。
    REQUIRE(!ConfigSync::IsSyncableKey(Id("ai_assistant", "token")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("ai_assistant", "token_openai")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("voice_input", "asr_token")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("voice_input", "polish_token_deepseek")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("tencent_tmt", "secret_key")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("niutrans", "apikey")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("sync", "password")));

    // 段本身不在白名单里的一律不同步：凭证段、代理段、同步段自己。
    REQUIRE(!ConfigSync::IsSyncableKey(Id("voice_input", "asr_provider")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("ai_assistant", "provider")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("network", "proxy_server")));
    REQUIRE(!ConfigSync::IsSyncableKey(Id("sync", "url")));

    // 格式不对的 id 不该崩，也不该被当成可同步项。
    REQUIRE(!ConfigSync::IsSyncableKey("no-separator"));
}

TEST_CASE(config_sync_snapshot_round_trips_through_text)
{
    std::map<std::string, std::string> snapshot = {
        {Id("general", "diagnostic_log"), "true"},
        {Id("appearance", "theme_mode"), "\"dark\""},
        // 带空格和等号的值原样保留，解析时只按第一个 '=' 切。
        {Id("skin", "candidate_skin"), "\"a = b\""},
    };
    const std::string text = ConfigSync::SerializeSnapshot(snapshot);
    REQUIRE(text.rfind("# MetasequoiaIME 配置同步 v1", 0) == 0);

    std::map<std::string, std::string> parsed;
    REQUIRE(ConfigSync::ParseSnapshot(text, parsed));
    REQUIRE_EQ(parsed.size(), snapshot.size());
    for (const auto &[key, value] : snapshot)
    {
        REQUIRE(parsed.count(key) == 1);
        REQUIRE_EQ(parsed.at(key), value);
    }
}

TEST_CASE(config_sync_snapshot_rejects_malformed_lines)
{
    std::map<std::string, std::string> parsed;
    REQUIRE(!ConfigSync::ParseSnapshot("这不是一个赋值行\n", parsed));
    // 奇数长度的十六进制不是合法的键。
    REQUIRE(!ConfigSync::ParseSnapshot("abc = true\n", parsed));
    // 注释和空行被跳过，空文件是合法的（远端还没建）。
    REQUIRE(ConfigSync::ParseSnapshot("# 只有注释\n\n", parsed));
    REQUIRE(parsed.empty());
}

TEST_CASE(config_sync_merge_takes_each_side_when_only_one_moved)
{
    const std::string a = Id("general", "diagnostic_log");
    const std::string b = Id("appearance", "theme_mode");
    const std::map<std::string, std::string> baseline = {{a, "false"}, {b, "\"dark\""}};
    const std::map<std::string, std::string> local = {{a, "true"}, {b, "\"dark\""}};    // 只有本机改了 a
    const std::map<std::string, std::string> remote = {{a, "false"}, {b, "\"light\""}}; // 只有远端改了 b

    const auto plan = ConfigSync::ComputeMerge(local, baseline, remote);
    REQUIRE(plan.conflicts.empty());
    REQUIRE_EQ(plan.push.size(), static_cast<size_t>(1));
    REQUIRE_EQ(plan.push.at(a), "true");
    REQUIRE_EQ(plan.apply.size(), static_cast<size_t>(1));
    REQUIRE_EQ(plan.apply.at(b), "\"light\"");
}

TEST_CASE(config_sync_merge_reports_real_conflicts_only)
{
    const std::string key = Id("general", "diagnostic_log");
    const std::map<std::string, std::string> baseline = {{key, "false"}};

    // 两边都改成了同一个值：不算冲突。
    {
        const auto plan = ConfigSync::ComputeMerge({{key, "true"}}, baseline, {{key, "true"}});
        REQUIRE(plan.conflicts.empty());
        REQUIRE(plan.push.empty());
        REQUIRE(plan.apply.empty());
    }
    // 两边改成不同的值：冲突，且不进 apply/push，等用户裁决。
    {
        const auto plan = ConfigSync::ComputeMerge({{key, "true"}}, baseline, {{key, "0"}});
        REQUIRE_EQ(plan.conflicts.size(), static_cast<size_t>(1));
        REQUIRE_EQ(plan.conflicts.front(), key);
        REQUIRE(plan.push.empty());
        REQUIRE(plan.apply.empty());
    }
}

TEST_CASE(config_sync_merge_handles_first_run_without_baseline)
{
    const std::string key = Id("general", "diagnostic_log");
    // 没有基线时，远端已有的值按「远端新键」处理，本地没有这个键就跟进；
    // 本地也有的同一个键两边不同则算冲突——第一次同步就只能问用户。
    const auto plan = ConfigSync::ComputeMerge({}, {}, {{key, "true"}});
    REQUIRE_EQ(plan.apply.size(), static_cast<size_t>(1));
    REQUIRE_EQ(plan.apply.at(key), "true");

    const auto conflict = ConfigSync::ComputeMerge({{key, "false"}}, {}, {{key, "true"}});
    REQUIRE_EQ(conflict.conflicts.size(), static_cast<size_t>(1));
}

TEST_CASE(config_sync_merge_noops_when_nothing_moved)
{
    const std::string key = Id("general", "diagnostic_log");
    const std::map<std::string, std::string> same = {{key, "true"}};
    const auto plan = ConfigSync::ComputeMerge(same, same, same);
    REQUIRE(plan.conflicts.empty());
    REQUIRE(plan.push.empty());
    REQUIRE(plan.apply.empty());
}

TEST_CASE(config_sync_target_path_is_built_from_a_directory)
{
    // RemoteUrl 不在公开头里，这里对它属于的规则做一次断言：用户填目录、程序拼文件名。
    // 目录带不带结尾斜杠都要拼出同一个地址，填的已经带文件名时不能拼第二遍。
    struct Case
    {
        const char *input;
        const char *expected;
    };
    const Case cases[] = {
        {"https://dav.example.com/msime", "https://dav.example.com/msime/metasequoiaime-config.toml"},
        {"https://dav.example.com/msime/", "https://dav.example.com/msime/metasequoiaime-config.toml"},
        {"https://dav.example.com/msime/metasequoiaime-config.toml",
         "https://dav.example.com/msime/metasequoiaime-config.toml"},
    };
    for (const auto &item : cases)
    {
        const std::string url = ConfigSync::RemoteTargetForTests(item.input);
        REQUIRE_EQ(url, std::string(item.expected));
    }
}
