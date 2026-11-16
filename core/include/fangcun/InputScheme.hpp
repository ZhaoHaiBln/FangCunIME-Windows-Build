#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fangcun {

enum class InputScheme {
    Flypy,
    Pinyin,
    NineKey,
};

inline std::string_view schemeLabel(InputScheme scheme) {
    switch (scheme) {
        case InputScheme::Flypy: return "小鹤双拼";
        case InputScheme::Pinyin: return "全拼";
        case InputScheme::NineKey: return "九宫格";
    }
    return "小鹤双拼";
}

/**
 * Normalization shared by database lookup and ranking, including explicit syllable breaks.
 */
namespace PinyinCode {
    std::string normalize(std::string_view input);
    std::string compact(std::string_view input);
    bool valid(std::string_view input, InputScheme scheme);
    std::string digits(std::string_view pinyin);
    std::optional<std::string> boundaryPrefix(std::string_view input);
} // namespace PinyinCode

} // namespace fangcun
