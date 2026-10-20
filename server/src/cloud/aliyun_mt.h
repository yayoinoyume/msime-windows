#pragma once

#include <string>
#include <utility>
#include <vector>

namespace AliyunMt
{
// 阿里云 AccessKey ID / AccessKey Secret。控制台「机器翻译」通用版按源文本字符计费，
// 主账号每月 100 万字符免费。
struct Credentials
{
    std::string access_key_id;
    std::string access_key_secret;
};

// Both credentials must be present and non-placeholder before requests are sent.
bool IsUsableCredentials(const Credentials &credentials);

// RFC 3986 percent-encoding as the RPC signature spec defines it: only A-Z a-z 0-9 - _ . ~
// stay literal. Exposed for tests.
// 注释：错误码在顶层 Code（数字或字符串），非 200 时整批作废，见 ParseBatchResponse。
std::string PercentEncode(const std::string &value);

// Signs an RPC request: sort every parameter (Signature excluded) by name, percent-encode and
// join as "k=v&...", then Base64(HMAC-SHA1(secret + "&", "POST&%2F&" + encoded)). Exposed for
// tests.
std::string BuildSignature(const Credentials &credentials, const std::string &http_method,
                           const std::vector<std::pair<std::string, std::string>> &parameters);

// Index of a batch entry's "index" field back to its position in the request. Entries with a
// non-200 per-item code or without a usable "translated" stay empty. Exposed for tests.
std::vector<std::string> ParseBatchResponse(const std::string &response, size_t count);

// GetBatchTranslate 的 SourceText 参数：key 是批次内下标（"0"、"1"…），不是 texts 里的全局
// 下标。分批发时 ParseBatchResponse 只拿到本批的条数，用全局下标会让第二批起每一项都越界。
// Exposed for tests.
std::string BuildBatchSourceText(const std::vector<std::string> &texts, size_t begin, size_t end);

// Calls GetBatchTranslate (通用版) once per batch of at most 50 texts. Empty entries mean that
// item failed; a result shorter than texts means the batch time budget ran out and the missing
// tail was never sent.
std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);
} // namespace AliyunMt
