// The portable twin of the local-service cases in test_ai_local_service.cpp: only what parses
// the model's answer, so this file depends on no vcpkg package and no Windows header and the
// sanitizers job can build it on Linux in seconds.
#include "ai/ai_candidate_text.h"
#include "tests/includes/test_framework.h"

TEST_CASE(portable_candidate_extraction_reads_the_documented_envelope)
{
    REQUIRE(FirstCandidateText(R"({"candidates":[{"text":"你好"}]})") == "你好");
    // Field order is whatever the model emitted.
    REQUIRE(FirstCandidateText(R"({"candidates":[{"confidence":0.9,"text":"广州"}]})") == "广州");
    // A bare string member carries the candidate too.
    REQUIRE(FirstCandidateText(R"({"candidates":["北京"]})") == "北京");
    // Regression guard: the array must be located through the "candidates" key rather than by the
    // first bracket in the document. Both shapes below happen to survive the naive scan today,
    // so these assertions protect the key-based lookup against a future simplification.
    REQUIRE(FirstCandidateText(R"({"prompt":"请[按{s}顺序]输出","candidates":[{"text":"上海"}]})") == "上海");
    REQUIRE(FirstCandidateText(R"({"sys":"示例", "text":"诱饵", "candidates":[{"text":"广州"}]})") == "广州");
}

TEST_CASE(portable_candidate_extraction_survives_local_model_shapes)
{
    // The fence is the most common deviation from the documented envelope.
    REQUIRE(FirstCandidateText("```json\n{\"candidates\":[{\"text\":\"上海\"}]}\n```") == "上海");
    // A lead-in sentence must not derail the scan.
    REQUIRE(FirstCandidateText("好的，结果如下：\n\n{\"candidates\":[{\"text\":\"学习\"}]}") == "学习");
    // Prose that ignores response_format still carries the answer.
    REQUIRE(FirstCandidateText("你好") == "你好");
    REQUIRE(FirstCandidateText("1. 上海") == "上海");
    REQUIRE(FirstCandidateText("- 你好") == "你好");
    REQUIRE(FirstCandidateText("候选: 你好") == "你好");
    // The limit counts characters, not UTF-8 bytes.
    REQUIRE(FirstCandidateText("中华人民共和国") == "中华人民共和国");
}

TEST_CASE(portable_candidate_extraction_declines_instead_of_guessing)
{
    REQUIRE(FirstCandidateText("").empty());
    REQUIRE(FirstCandidateText("   \n  ").empty());
    // Structured data the strict path could not read: quoting any part of it into the document
    // would commit a JSON key or a bracket as the candidate.
    REQUIRE(FirstCandidateText(R"({"answer":"你好"})").empty());
    REQUIRE(FirstCandidateText(R"({"candidates":[]})").empty());
    REQUIRE(FirstCandidateText(R"({"text":"未闭合}")").empty());
    // A sentence is a refusal, not a candidate for a couple of syllables.
    REQUIRE(FirstCandidateText("我无法完成这个请求，因为这不是一个合法的输入序列，请检查后重试。").empty());
}
