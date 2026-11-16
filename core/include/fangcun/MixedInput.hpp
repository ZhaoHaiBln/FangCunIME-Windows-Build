#pragma once

#include "InputScheme.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace fangcun {

/**
 * Small offline English supplement (339 everyday words);
 * preserves original Latin case and keeps Chinese ambiguities Chinese-first.
 */
namespace MixedInput {
    std::vector<std::string> candidates(
        std::string_view input,
        const std::vector<std::string>& chinese,
        InputScheme scheme);

    bool isEnglish(std::string_view text);
    bool preferEnglish(std::string_view input);
} // namespace MixedInput

} // namespace fangcun
