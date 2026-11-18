#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
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

} // namespace fangcun
