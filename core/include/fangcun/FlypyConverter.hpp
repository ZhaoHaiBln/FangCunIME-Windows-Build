#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fangcun {

/**
 * Pure pinyin -> Xiaohe FlyPy conversion using generated rime-ice algebra rules.
 */
namespace FlypyConverter {
    std::optional<std::string> toCode(std::string_view pinyin);
} // namespace FlypyConverter

} // namespace fangcun
