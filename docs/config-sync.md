# 配置同步（WebDAV）

> 把一台机器上调好的设置带到其他机器上，不用逐个开关重设。只同步**设置项**，不同步凭证。

## 目录

- [能同步什么、不能同步什么](#能同步什么不能同步什么)
- [配置](#配置)
- [在设置页里用](#在设置页里用)
- [冲突怎么处理](#冲突怎么处理)
- [数据目录里的文件](#数据目录里的文件)
- [实现说明](#实现说明)

## 能同步什么、不能同步什么

同步的单位是**配置键**，不是整个文件。参与同步的是 `config.toml` 里这些段：

`[general]` `[appearance]` `[input]` `[language_bar]` `[helpcode]` `[quanpin]` `[keybindings]`
`[dictionary]` `[skin]` `[soft_keyboard]` `[utility]` `[statistics]` `[frequency_adjustment]`

| 不同步 | 为什么 |
|---|---|
| 凭证（`token*`、`api_key`、`asr_token*`、`polish_token*`、`secret_id`、`secret_key`、`app_id`、`password`） | 落盘时用 DPAPI 加密，绑定本机本账户；换机器解不开，代码里按「没填」处理。同步密文只会得到一堆空值，所以干脆不传 |
| `[sync]` 段本身 | 把 WebDAV 密码同步到别的机器没有意义，两台机器还会互相改写地址 |
| `[network]` 代理设置 | 各机器网络环境不同 |
| 词库（`msime.db` / `english.db` / `others.db` / `dict_japanese.dat`） | 随安装包自带，几百 MB |
| `msime_user.db`（用户词库）、`stats.db` | SQLite，两台机器同时挂同一份会损坏 |
| `models\`、`skins\`、`helpcodes\custom\`、`shuangpin\custom\` | 目录型数据，各自按需安装 |
| `config.default.toml`、`config.base.toml` | 前者是本次安装的出厂模板，后者是升级合并的基线；把别人的基线带过来会让升级时的「哪些键用户改过」判断出错 |

**换机器后仍要手动做的**：重新填一遍各家服务密钥，装好需要的词库和模型包。设置项本身拉一次就到位。

## 配置

`config.toml` 里的 `[sync]` 段。正常情况不用手改，设置页会写。

```toml
[sync]
enabled = false                        # 打开后按 interval 自动双向同步
url = "https://dav.jianguoyun.com/dav/msime"   # 填到目录为止
user = ""
password = ""                          # 落盘后自动变成 dpapi: 密文
interval = "15m"                       # manual / 5m / 15m / 30m / 60m
```

- `url` 只接受 `http://` 或 `https://`。填目录即可，文件名固定为 `metasequoiaime-config.toml` 自动拼在后面；目录需要先在 WebDAV 里建好。
- 密码与 `[ai_assistant]`、`[voice_input]` 下的令牌走同一套 DPAPI 加密，明文写进 `config.toml` 后由 Server 在下次加载时就地加密。
- 改完不需要重启：Server 每 300 ms 检查配置文件的修改时间，`[sync]` 段变化会让后台定时器按新间隔重排。

## 在设置页里用

设置 → **配置同步**。

| 控件 | 作用 |
|---|---|
| 在多台机器间同步设置 | 总开关 |
| 自动同步间隔 | 「仅手动」以外都会在后台按时同步 |
| WebDAV 目录 / 用户名 / 密码 | 连接信息 |
| 立即同步 | 双向合并 |
| 上传本机设置 | 单向：用本机整份覆盖云端 |
| 拉取云端设置 | 单向：只应用云端较新的键，不把本机的推上去 |
| 解决冲突 | 有冲突时才可点，二选一 |

后台定时器跑在 Server 进程里，设置窗口关掉后照样工作。

## 冲突怎么处理

同步是三方的：**本机当前值**、**上次同步时的本机值**（基线）、**云端值**。

| 情况 | 结果 |
|---|---|
| 只有一边改了某个键 | 用改过的那一边，不打扰用户 |
| 两边改成了一样的值 | 视同没冲突 |
| 两边改成不同值 | **冲突**：不写任何一边，停下来问用户 |

出冲突时，本机版本和云端版本会各存一份到数据目录：

```
sync_conflict_local_<unix时间戳>.txt
sync_conflict_remote_<unix时间戳>.txt
```

然后点「解决冲突」选保留哪一边。**不会静默丢配置**——这是刻意的取舍：第一版宁可多点一次，也不猜你想要哪边的值。

刚装好、还没有基线时，如果同一个键本机和云端都有且不同，也算冲突（只能问用户）。云端较新、本机没有的键会直接跟随。

## 数据目录里的文件

| 文件 | 作用 |
|---|---|
| `sync_state.txt` | 基线：上次同步成功时的本机键值。删掉它不会丢配置，只是下一次同步会退化成「没有基线」的保守合并 |
| `sync_conflict_local_*.txt` / `sync_conflict_remote_*.txt` | 冲突时留的证据，可以手动看也可以直接删 |
| `metasequoiaime-config.toml`（在 WebDAV 上） | 云端快照。一行一个键，键名是 TOML 赋值 id 的十六进制，值就是 `config.toml` 里的原始字面量，可以 diff |

云端文件长这样：

```
# MetasequoiaIME 配置同步 v1
67656e6572616c01646961676e6f737469635f6c6f67 = true
617070656172616e6365017468656d655f6d6f6465 = "dark"
```

十六进制是「段名 + `\x01` + 键名」的 UTF-8 字节。这样键名里的点号、花括号等不会和 TOML 的键语法打架，合并时也不需要重新序列化——值直接照抄，格式和注释都留在各自的本地文件里。

## 实现说明

| 位置 | 内容 |
|---|---|
| `server/src/sync/config_sync.h` | 对外接口：`Run` / `ResolveConflict` / `StartScheduler` / `PendingConflict` |
| `server/src/sync/config_sync_merge.cpp` | 纯逻辑：参与范围判定、云端文件序列化、三方合并。不碰网络，单测覆盖在这里 |
| `server/src/sync/config_sync_webdav.cpp` | WebDAV GET/PUT、合并落地、冲突留证、后台定时器 |
| `server/src/settings/native/native_page_sync.cpp` | 设置页 |
| `server/tests/src/test_config_sync.cpp` | 范围判定、文件往返、合并与冲突的 7 个用例 |

网络请求走 `NetworkProxy::ApplyToCurl`，和云输入、模型下载、AI 联想共用 `[network]` 的代理配置（系统 / 直连 / 自定义）。写本地配置走 `ime_config_detail::WriteConfiguredValues`，因此凭证加密、格式保留和「通知 Server 重载」都由现有入口负责，同步层不重复实现。

一个已知的取舍：同一时刻只允许一次同步（`Run` 里 `try_to_lock`），第二个请求直接返回「上一次同步还没结束」，不排队。手动按钮和定时器撞上时是这个表现，够用就好。

另有一个已知限制：**同步不解决凭证跨机器的问题**。DPAPI 密文换机器解不开，代码按空值处理。如果将来要做凭据同步，需要先引入用户口令派生的密钥，那是另一件事。
