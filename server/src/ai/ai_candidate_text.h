#pragma once

#include <string>

// Extracts the candidate a Chat Completions answer carries, deliberately without a JSON parser
// and without any platform or vcpkg header, so the portable test target can build it.
//
// The documented envelope is {"candidates":[{"text":"..."}]}, but a model is free to wrap it in a
// Markdown fence, precede it with a sentence, number it, or answer in bare prose. Every shape
// that used to fall through json::parse silently lost the suggestion. Anything that is not a
// recognizable answer yields an empty string, so nothing malformed is ever committed.
std::string FirstCandidateText(const std::string &content);
