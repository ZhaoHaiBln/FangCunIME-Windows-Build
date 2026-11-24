#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace fangcun {

inline void fitHorizontalCandidateWidths(std::vector<float>& widths, float available) {
    available = std::max(available, 0.0f);
    if (widths.empty()) return;
    const float otherDemand = std::accumulate(widths.begin() + 1, widths.end(), 0.0f);
    if (widths.front() + otherDemand <= available) return;
    if (widths.size() == 1) {
        widths.front() = available;
        return;
    }

    // Preserve the enlarged first word when it fits without crowding out the rest.
    const float firstBudget = std::max(available * 0.4f, available - otherDemand);
    widths.front() = std::min(widths.front(), firstBudget);
    available -= widths.front();

    // Keep short words intact, then share the remaining width among the long ones.
    std::vector<float> sorted(widths.begin() + 1, widths.end());
    std::sort(sorted.begin(), sorted.end());
    float cap = 0.0f;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        cap = available / static_cast<float>(sorted.size() - i);
        if (sorted[i] >= cap) break;
        available -= sorted[i];
    }
    for (std::size_t i = 1; i < widths.size(); ++i) widths[i] = std::min(widths[i], cap);
}

// Keep the part nearest the typing caret visible without changing the source
// composition or candidate/window geometry. Measure includes the leading mark.
template <typename Measure>
std::wstring fitPreeditTail(std::wstring_view text, float available, Measure measure) {
    if (text.empty() || !(available > 0.0f)) return {};
    std::wstring full(text);
    if (measure(full) <= available) return full;

    const std::wstring ellipsis(1, L'\u2026');
    if (!(measure(ellipsis) <= available)) return {};

    std::vector<std::size_t> suffixes;
    suffixes.reserve(text.size());
    for (std::size_t position = 0; position < text.size();) {
        const auto first = static_cast<unsigned int>(text[position++]);
        if (first >= 0xD800 && first <= 0xDBFF && position < text.size()) {
            const auto second = static_cast<unsigned int>(text[position]);
            if (second >= 0xDC00 && second <= 0xDFFF) ++position;
        }
        suffixes.push_back(position);
    }
    const auto suffix = [&](std::size_t index) {
        std::wstring result(ellipsis);
        result.append(text.substr(suffixes[index]));
        return result;
    };

    // Bounded logarithmic measurements for long input; the last suffix is the
    // already-measured ellipsis, so the search always has a fitting endpoint.
    std::size_t low = 0;
    std::size_t high = suffixes.size() - 1;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (measure(suffix(middle)) <= available) high = middle;
        else low = middle + 1;
    }
    return suffix(low);
}

} // namespace fangcun
