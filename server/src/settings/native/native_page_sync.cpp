#include "settings/native/native_page_help_widgets.h"
#include "settings/native/native_page_voice_fields.h"

#include "msimeui/DeviceResources.h"

#include <algorithm>

// partials/sync-settings.html + modules/sync-settings.ts
namespace native_settings
{
namespace
{
// [sync] 状态码，与 sync/config_sync.h 的 ConfigSync::Status 一一对应。
constexpr int kStatusDisabled = 0;
constexpr int kStatusUpToDate = 1;
constexpr int kStatusConflict = 5;

// 说明文字最多 52em，和 .quick-phrase-description 一致，避免一行拉满 900px。
constexpr float kDescriptionEm = 52.0f;

VisualPtr TitleWithText(const std::wstring &title, const std::wstring &description)
{
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeTitle(title));
    if (!description.empty())
    {
        auto desc = MakeDescription(description);
        desc->SetMaxWidth(kDescriptionEm * kFontCaption);
        column->Add(desc);
    }
    return column;
}

// 左说明 + 右控件，控件与首行文字对齐（高控件不和整块文字垂直居中）。
class AlignedRow : public msimeui::Panel
{
  public:
    AlignedRow(VisualPtr left, VisualPtr control) : left_(std::move(left)), control_(std::move(control))
    {
        AddChild(left_);
        AddChild(control_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        controlSize_ = control_->MeasureInLayout(availableSize);
        leftSize_ = left_->MeasureInLayout(
            {std::max(0.0f, availableSize.width - controlSize_.width - kGap), availableSize.height});
        return {availableSize.width, std::max(leftSize_.height, controlSize_.height)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        control_->ArrangeInLayout(
            {finalRect.x + finalRect.width - controlSize_.width, finalRect.y, controlSize_.width, controlSize_.height});
        left_->ArrangeInLayout(
            {finalRect.x, finalRect.y, std::max(0.0f, finalRect.width - controlSize_.width - kGap), leftSize_.height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        for (const auto &child : children_)
            child->Render(dr);
    }

  private:
    static constexpr float kGap = 10.0f;
    VisualPtr left_;
    VisualPtr control_;
    SizeF leftSize_ = {};
    SizeF controlSize_ = {};
};

// 一段纯文字，撑开卡片的内边距用。
VisualPtr NoteCard(const std::wstring &title, const std::wstring &text)
{
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeSubTitle(title));
    auto desc = MakeDescription(text);
    desc->SetMaxWidth(kDescriptionEm * kFontCaption);
    desc->SetMargin({0.0f, 6.0f, 0.0f, 0.0f});
    column->Add(desc);
    return column;
}
} // namespace

VisualPtr BuildSyncPage(PageContext &context)
{
    PageBuilder page(L"配置同步");

    // 总开关。
    page.AddSection(MakeRow(L"在多台机器间同步设置",
                            L"把本机改过的设置存到你的 WebDAV 上，其他机器装好后拉一次就一致。"
                            L"凭证（各家 API Key、语音 token、WebDAV 密码）、词库、模型包不参与同步："
                            L"它们用 DPAPI 加密且绑定本机本账户，换机器解不开，仍需各自填一次。",
                            BindToggle(context, "sync.enabled")));

    // 自动同步间隔。
    {
        Dropdown::Options dropdown;
        dropdown.minWidth = 120.0f;
        auto interval = BindDropdown(context, "sync.interval",
                                     Options({{"manual", L"仅手动"},
                                              {"5m", L"每 5 分钟"},
                                              {"15m", L"每 15 分钟"},
                                              {"30m", L"每 30 分钟"},
                                              {"60m", L"每 60 分钟"}}),
                                     dropdown);
        page.AddSection(MakeRow(L"自动同步间隔",
                                L"「仅手动」时只有下面的按钮会触发同步。其余间隔在后台双向同步，"
                                L"遇到冲突只提示、不覆盖任何一边。",
                                interval));
    }

    // WebDAV 连接信息。
    {
        auto grid = std::make_shared<FormGrid>();

        TextField::Options urlOptions;
        urlOptions.placeholder = L"https://dav.jianguoyun.com/dav/msime";
        grid->Add(std::make_shared<Field>(L"WebDAV 目录", BindText(context, "sync.url", urlOptions)), true);

        TextField::Options userOptions;
        userOptions.placeholder = L"登录用户名";
        grid->Add(std::make_shared<Field>(L"用户名", BindText(context, "sync.user", userOptions)));

        TextField::Options passwordOptions;
        passwordOptions.password = true;
        passwordOptions.revealButton = true;
        passwordOptions.placeholder = L"登录密码或应用专用密码";
        grid->Add(std::make_shared<Field>(L"密码", BindText(context, "sync.password", passwordOptions)));

        auto hint = MakeDescription(L"填到目录为止（例如 .../dav/msime），文件名会自动拼成 "
                                    L"metasequoiaime-config.toml，目录需要先自己建好。密码和其他凭证"
                                    L"一样加密后才写进 config.toml。");
        hint->SetMaxWidth(kDescriptionEm * kFontCaption);

        auto column = std::make_shared<Column>(0.0f);
        column->Add(hint);
        grid->SetMargin({0.0f, 14.0f, 0.0f, 0.0f});
        column->Add(grid);
        page.AddSection(std::make_shared<PaddedSection>(column, Thickness{24.0f, 20.0f, 24.0f, 20.0f}));
    }

    // 手动动作与结果。
    {
        auto status = std::make_shared<StatusText>();
        auto sync_now = std::make_shared<Button>(L"立即同步", ButtonKind::Primary);
        auto upload = std::make_shared<Button>(L"上传本机设置", ButtonKind::Neutral);
        auto download = std::make_shared<Button>(L"拉取云端设置", ButtonKind::Neutral);
        auto resolve = std::make_shared<Button>(L"解决冲突", ButtonKind::Neutral);
        resolve->SetDisabled(true);

        auto actions = std::make_shared<Row>(10.0f);
        actions->Add(sync_now);
        actions->Add(upload);
        actions->Add(download);
        actions->Add(resolve);

        // 发出去之前先把上一次的结果清掉，免得「同步中」和上一轮结果同屏。
        const auto post = [&context, status](const char *action) {
            status->Clear();
            context.model.Post("configSyncRequest",
                               {{"requestId", context.model.NewRequestId("sync")}, {"action", action}});
        };
        sync_now->SetOnClick([post]() { post("auto"); });
        upload->SetOnClick([post]() { post("upload"); });
        download->SetOnClick([post]() { post("download"); });

        context.OnMessage("configSyncResponse", [status, resolve](const Json &message) {
            const int code = message.value("status", kStatusDisabled);
            status->Set(Widen(message.value("message", std::string())), code == kStatusConflict ? "error" : "success");
            resolve->SetDisabled(message.value("conflicts", 0) <= 0);
        });

        // 有冲突才让按钮可点；点开是二选一，两份副本已经写进数据目录，不会静默丢配置。
        resolve->SetOnClick([&context]() {
            context.host.Confirm(
                L"本机和云端改过同一批设置。保留本机版本会把这些键覆盖到云端，保留云端版本会覆盖本机。"
                L"两份副本都已存到数据目录，文件名带 sync_conflict_ 前缀。\n\n保留哪一边？",
                [&context](bool confirmed) {
                    context.model.Post("configSyncRequest", {{"requestId", context.model.NewRequestId("sync")},
                                                             {"action", confirmed ? "keepLocal" : "keepRemote"}});
                },
                L"解决冲突", L"保留本机", L"保留云端");
        });
        auto column = std::make_shared<Column>(0.0f);
        column->Add(actions);
        column->Add(status);
        page.AddRaw(std::make_shared<PageCard>(
                        AlignedRow(TitleWithText(L"手动同步", L"「立即同步」双向合并：云端没有就上传，"
                                                              L"本机没改过的键跟着云端走，两边都改过的同一个"
                                                              L"键会停下来问你。上传 / 拉取是单向的，会整份"
                                                              L"覆盖另一端。"),
                                   column),
                        PageCard::Style::Section, Thickness{24.0f, 20.0f, 24.0f, 20.0f}),
                    18.0f);
    }

    // 排除清单写清楚，省得用户猜为什么某些设置没跟过来。
    page.AddSection(std::make_shared<PaddedSection>(
        MakeRows(
            {NoteCard(L"不同步的内容", L"各家 API Key / token / App ID、语音与 AI 凭据、WebDAV 密码（都绑定本机账户）、"
                                       L"用户词库与统计库、下载的模型包、皮肤与辅助码目录、config.base.toml。"),
             NoteCard(L"换机器后要手动做的",
                      L"重新填一遍各家服务密钥，装好需要的词库和模型包。设置项本身拉一次就到位。")}),
        Thickness{24.0f, 16.0f, 24.0f, 16.0f}));

    return page.Build();
}
} // namespace native_settings
