#include "ai_assistant_request.h"
#include "chat_completions.h"
#include "chat_request_params.h"

#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking(config.provider, config.endpoint, config.thinking) ||
                   ChatCompletions::IsOpenAiReasoningRequest(config.provider, config.model)
               ? ChatCompletions::kReasoningTimeoutMs
               : ChatCompletions::kDefaultTimeoutMs;
}
} // namespace AiAssistant::detail
