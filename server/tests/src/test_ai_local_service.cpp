// Local model servers answer unauthenticated, and they wrap JSON far less predictably than the
// hosted providers do. Both facts used to make the suggestion disappear without a trace: a blank
// token never reached the transport, and a fenced or prose answer failed json::parse outright.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ai/ai_assistant_request.h"
#include "tests/includes/test_framework.h"

namespace
{
std::string Candidate(std::string content)
{
    return AiAssistant::detail::FirstCandidateText(content);
}
} // namespace

TEST_CASE(local_service_parses_plain_json_answer)
{
    REQUIRE(Candidate(R"({"candidates":[{"text":"你好"}]})") == "你好");
}

TEST_CASE(local_service_parses_fenced_json_answer)
{
    // A fenced block is the single most common local-model deviation.
    REQUIRE(Candidate("```json\n{\"candidates\":[{\"text\":\"上海\"}]}\n```") == "上海");
    // A lead-in sentence before the object must not derail the parse either.
    REQUIRE(Candidate("好的，结果如下：\n\n{\"candidates\":[{\"text\":\"学习\"}]}") == "学习");
}

TEST_CASE(local_service_parses_bare_text_answer)
{
    // response_format is a request, not a guarantee; the answer still carries the candidate.
    REQUIRE(Candidate("你好") == "你好");
    REQUIRE(Candidate("好的") == "好的");
    REQUIRE(Candidate("1. 上海") == "上海");
    REQUIRE(Candidate("12) 天津") == "天津");
    REQUIRE(Candidate("- 你好") == "你好");
    REQUIRE(Candidate("候选: 你好") == "你好");
    REQUIRE(Candidate("你好\n世界") == "你好");
    // The limit counts characters, not bytes: a UTF-8 Chinese character is three of them.
    REQUIRE(Candidate("中华人民共和国") == "中华人民共和国");
}

TEST_CASE(local_service_declines_rather_than_committing_junk)
{
    REQUIRE(Candidate("").empty());
    REQUIRE(Candidate("   \n  ").empty());
    // Structured data the strict path could not read. Salvaging a quoted run inside it would put
    // a JSON key or a bracket in the document, so the whole thing is declined.
    REQUIRE(Candidate(R"({"answer":"你好"})").empty());
    REQUIRE(Candidate(R"({"candidates":[]})").empty());
    REQUIRE(Candidate(R"(["a","b"])").empty());
    REQUIRE(Candidate(R"("上海")").empty());
    // A sentence is a refusal or an explanation, not a candidate for a couple of syllables.
    REQUIRE(Candidate("我无法完成这个请求，因为这不是一个合法的输入序列，请检查后重试。").empty());
}

TEST_CASE(local_service_request_stays_a_plain_openai_compatible_shape)
{
    AiAssistant::Request request;
    request.pinyin_segments = {"ni", "hao"};
    request.config.provider = "ollama";
    request.config.endpoint = "http://127.0.0.1:11434/v1/chat/completions";
    request.config.model = "qwen2.5:7b";
    request.config.prompt = "Return the best candidate.";

    const auto body = AiAssistant::detail::BuildRequestBody(request);
    // A local service gets no vendor fields it never asked for.
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_completion_tokens"));
    REQUIRE(!body.contains("thinking"));
    REQUIRE(!body.contains("enable_thinking"));
    REQUIRE(!body.contains("reasoning_effort"));
    // JSON mode is what makes the answer machine-readable, so it stays on.
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
}

TEST_CASE(local_service_timeout_matches_the_plain_default)
{
    AiAssistantConfig config;
    config.provider = "ollama";
    config.endpoint = "http://127.0.0.1:11434/v1/chat/completions";
    config.model = "qwen2.5:7b";
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(config), 8000L);
}
