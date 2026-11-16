#include "fangcun/CandidateRanker.hpp"

#include <algorithm>
#include <unordered_set>

namespace fangcun::CandidateRanker {

int64_t score(const DictionaryEntry& entry, std::string_view input, int32_t userCount) {
    const int64_t learned = static_cast<int64_t>(std::clamp(userCount, 0, 1000)) * 100'000'000'000'000LL;
    const int64_t exact = (entry.code == input) ? 1'000'000'000'000'000'000LL : 0LL;
    const int64_t dictionary = std::max<int64_t>(0, entry.weight) * 128LL;
    const int64_t shorterFirst = 100 - std::clamp(entry.len, 0, 100);
    return learned + exact + dictionary + shorterFirst;
}

std::vector<std::string> rank(
    const std::vector<DictionaryEntry>& entries,
    std::string_view input,
    const std::unordered_map<std::string, int32_t>& frequencies) {
    struct Scored {
        const DictionaryEntry* entry;
        int64_t s;
    };

    std::vector<Scored> items;
    items.reserve(entries.size());
    for (const auto& e : entries) {
        auto it = frequencies.find(e.text);
        int32_t freq = (it != frequencies.end()) ? it->second : 0;
        items.push_back({&e, score(e, input, freq)});
    }

    std::sort(items.begin(), items.end(), [](const Scored& a, const Scored& b) {
        if (a.s != b.s) return a.s > b.s;
        return a.entry->text < b.entry->text;
    });

    std::vector<std::string> result;
    result.reserve(items.size());
    std::unordered_set<std::string> seen;
    seen.reserve(items.size());
    for (const auto& item : items) {
        if (seen.insert(item.entry->text).second) {
            result.push_back(item.entry->text);
        }
    }
    return result;
}

} // namespace fangcun::CandidateRanker
