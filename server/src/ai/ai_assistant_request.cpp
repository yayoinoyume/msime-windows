#include "ai_assistant_request.h"
#include "chat_completions.h"
#include "chat_request_params.h"

#include "voice-input/voice_providers.h"

#include <string_view>

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    // Which vendor answers is a property of the endpoint, not of the label in the settings UI: a
    // slot can hold any OpenAI-compatible endpoint, and DeepSeek's thinking.type makes MiniMax
    // reject the request outright instead of silently returning no suggestion.
    const auto vendor = VoiceInput::VendorForRequest(config.provider, config.endpoint);
    const bool openai_reasoning = ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model);
    const bool thinking = vendor == VoiceInput::ChatVendor::DeepSeek &&
                          VoiceInput::IsValidThinkingLevel(config.thinking) && config.thinking != "disabled";
    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"response_format", {{"type", "json_object"}}},
        {"messages",
         {{{"role", "system"}, {"content", config.prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
    if (!openai_reasoning)
        body["temperature"] = 0.2;
    // Reasoning tokens count against the output limit and can leave no room for JSON.
    ChatCompletions::SetTokenLimit(body, config.provider, thinking || openai_reasoning ? 8192 : 512);
    if (thinking)
    {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = config.thinking;
    }
    else if (openai_reasoning)
    {
        body["reasoning_effort"] = ChatCompletions::kOpenAiReasoningEffort;
    }
    else if (vendor == VoiceInput::ChatVendor::DeepSeek)
    {
        // DeepSeek thinking can add hundreds of reasoning tokens and noticeably delay
        // an IME suggestion. Every other service gets a plain OpenAI-compatible request.
        body["thinking"] = {{"type", "disabled"}};
    }
    return body;
}

namespace
{
// The outermost {...} block, so ```json fences and a lead-in sentence both survive. Empty when
// the text holds no braces at all, which is what makes the bare-text fallback reachable.
std::string_view OutermostJsonObject(std::string_view text)
{
    const std::size_t open = text.find('{');
    if (open == std::string_view::npos)
        return {};
    const std::size_t close = text.rfind('}');
    if (close == std::string_view::npos || close < open)
        return {};
    return text.substr(open, close - open + 1);
}

std::string_view TrimmedLine(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    text.remove_prefix(first);
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text.remove_suffix(text.size() - last - 1);
    return text;
}

// The candidate a model that ignored response_format still carries as bare text. Returning an
// empty string keeps every failure mode silent: a refusal, a code block or a raw JSON object
// must never be committed to the document as a candidate.
std::string FirstPlainLine(std::string_view text)
{
    if (const std::size_t newline = text.find_first_of("\r\n"); newline != std::string_view::npos)
        text = text.substr(0, newline);
    text = TrimmedLine(text);
    // Structured data we already failed to read: committing any part of it would put a JSON key,
    // a bracket or a value in the document. Guessing which quoted run is the answer loses to
    // just declining, because the strict path above already handled the answer when it was there.
    if (const std::size_t bracket = text.find_first_of("{}[]\"'`"); bracket != std::string_view::npos)
        return {};
    // Numbering and bullet prefixes are list decoration, not part of the candidate. The prefix is
    // compared as a whole word, because "候选:" is seven bytes and a byte-width test would never
    // match it.
    const std::size_t prefix_end = text.find_first_of(" \t");
    if (prefix_end != std::string_view::npos && prefix_end > 0)
    {
        const std::string_view prefix = text.substr(0, prefix_end);
        const bool is_marker = prefix == "-" || prefix == "*" || prefix == ">" || prefix == "#" || prefix == "##" ||
                               prefix == "→" || prefix == "候选:" || prefix == "候选：";
        // A numbering needs at least one digit alongside its punctuation, so a bare "..." or "、"
        // is left alone instead of being mistaken for one.
        const bool has_digit = prefix.find_first_of("0123456789") != std::string_view::npos;
        const bool is_numbering =
            has_digit && prefix.find_first_not_of("0123456789.、)）:：") == std::string_view::npos;
        if (is_marker || is_numbering)
            text = TrimmedLine(text.substr(prefix_end + 1));
    }
    // A candidate replaces a few syllables; a whole sentence is a refusal or an explanation.
    // Counted in characters, not bytes: a UTF-8 continuation byte is 0b10xxxxxx, so skipping
    // them keeps a 7-character name from tripping a byte-sized limit.
    const auto character_count = [](std::string_view value) {
        std::size_t count = 0;
        for (const char ch : value)
            if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
                ++count;
        return count;
    };
    if (text.empty() || character_count(text) > 20)
        return {};
    return std::string(text);
}

std::string CandidateFromText(std::string_view content)
{
    const std::string_view raw = TrimmedLine(content);
    if (raw.empty())
        return {};
    const auto candidate_from_json = [](std::string_view text) -> std::string {
        const auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
            return {};
        const auto list = parsed.find("candidates");
        if (list == parsed.end() || !list->is_array() || list->empty())
            return {};
        const auto first = list->begin();
        if (!first->is_object())
            return first->is_string() ? first->get<std::string>() : std::string{};
        const auto text_field = first->find("text");
        return text_field != first->end() && text_field->is_string() ? text_field->get<std::string>() : std::string{};
    };

    for (const std::string_view attempt : {raw, OutermostJsonObject(raw)})
    {
        if (attempt.empty())
            continue;
        const std::string found = candidate_from_json(attempt);
        if (!found.empty())
            return found;
    }
    return FirstPlainLine(raw);
}
} // namespace

long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking(config.provider, config.endpoint, config.thinking) ||
                   ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model)
               ? ChatCompletions::kReasoningTimeoutMs
               : ChatCompletions::kDefaultTimeoutMs;
}

std::string FirstCandidateText(const std::string &content)
{
    return CandidateFromText(content);
}
} // namespace AiAssistant::detail
