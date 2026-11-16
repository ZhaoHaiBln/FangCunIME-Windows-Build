#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fangcun {

struct DictionaryEntry {
    std::string code;
    std::string text;
    int32_t weight = 0;
    int32_t len = 0;
};

/**
 * Lexicographic priority: exact code > learned frequency > dictionary weight > shorter text.
 * Every component is bounded; the total remains safely below INT64_MAX.
 */
namespace CandidateRanker {
    int64_t score(const DictionaryEntry& entry, std::string_view input, int32_t userCount);
    std::vector<std::string> rank(
        const std::vector<DictionaryEntry>& entries,
        std::string_view input,
        const std::unordered_map<std::string, int32_t>& frequencies);
} // namespace CandidateRanker

} // namespace fangcun
