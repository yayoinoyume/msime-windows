#pragma once

#include "ai_assistant.h"

#include <nlohmann/json.hpp>

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request);
// DeepSeek thinking needs far more time than a plain completion before the answer arrives.
long RequestTimeoutMs(const AiAssistantConfig &config);

// The candidate text the suggestion router should apply, or an empty string when the model's
// answer carries none. Local models wrap JSON in Markdown fences, prepend a sentence or emit
// bare text far more often than the hosted ones do, and every one of those shapes used to fall
// through json::parse and lose the suggestion silently. An answer that is not the documented
// envelope yields an empty string rather than a guess, so nothing malformed is ever committed.
std::string FirstCandidateText(const std::string &content);
} // namespace AiAssistant::detail
