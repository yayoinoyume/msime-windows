#include "ai_candidate_text.h"

#include <optional>
#include <string_view>

#include <cstddef>

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

// candidates[0].text, without a JSON parser. The shape is fixed and shallow, so scanning for the
// first "candidates" array and the first "text" member after it is enough; a malformed document
// simply yields nothing. That keeps this function free of nlohmann, which matters because the
// portable test target has no vcpkg tree and cannot link it.
std::string CandidateFromJson(std::string_view text)
{
    // The value of the first "<key>" : <string> pair, or nullopt when the key is absent or its
    // value is not a string. A key must be followed by a colon, which is what tells a field apart
    // from prose that happens to mention the word.
    const auto first_string_value = [](std::string_view body, std::string_view key) -> std::optional<std::string> {
        const std::size_t key_at = body.find(key);
        if (key_at == std::string_view::npos)
            return std::nullopt;
        const std::size_t colon = body.find(':', key_at + key.size());
        if (colon == std::string_view::npos)
            return std::nullopt;
        const std::size_t open = body.find('"', colon + 1);
        if (open == std::string_view::npos)
            return std::nullopt;
        const std::size_t close = body.find('"', open + 1);
        // An unterminated literal is malformed; nothing here is worth guessing about.
        if (close == std::string_view::npos)
            return std::nullopt;
        return std::string(body.substr(open + 1, close - open - 1));
    };
    if (text.empty())
        return {};
    // The array is located through its own key rather than by the first "[" seen: a document that
    // carries other arrays before it (a prompt that illustrates a list, say) would otherwise hand
    // the scan the wrong members.
    const std::size_t candidates_key = text.find("\"candidates\"");
    if (candidates_key == std::string_view::npos)
        return {};
    const std::size_t array_open = text.find('[', candidates_key);
    if (array_open == std::string_view::npos)
        return {};
    std::string_view member = text.substr(array_open + 1);
    // An object member carries "text"; a bare string member is the value itself.
    if (const std::size_t first_object = member.find('{'); first_object != std::string_view::npos)
        return first_string_value(member.substr(first_object + 1), "\"text\"").value_or(std::string());
    const std::size_t open = member.find('"');
    if (open == std::string_view::npos)
        return {};
    const std::size_t close = member.find('"', open + 1);
    if (close == std::string_view::npos)
        return {};
    return std::string(member.substr(open + 1, close - open - 1));
}

std::string CandidateFromText(std::string_view content)
{
    const std::string_view raw = TrimmedLine(content);
    if (raw.empty())
        return {};
    for (const std::string_view attempt : {raw, OutermostJsonObject(raw)})
    {
        if (attempt.empty())
            continue;
        const std::string found = CandidateFromJson(attempt);
        if (!found.empty())
            return found;
    }
    return FirstPlainLine(raw);
}

} // namespace

std::string FirstCandidateText(const std::string &content)
{
    return CandidateFromText(content);
}
