#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fangcun::settings {

using Values = std::map<std::string, std::string>;

inline std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline bool entry(std::string_view line, std::string& key, std::string& value) {
    line = trim(line);
    if (line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);
    if (line.empty() || line.front() == '#' || line.front() == ';') return false;
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) return false;
    key = trim(line.substr(0, equals));
    value = trim(line.substr(equals + 1));
    return !key.empty();
}

inline Values parse(std::string_view text) {
    Values values;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        std::string key, value;
        if (entry(text.substr(begin, end - begin), key, value)) values[key] = value;
        begin = end;
    }
    return values;
}

// Change only owned keys. Unknown keys, comments and their line endings survive.
inline std::string merge(std::string_view text, const Values& updates) {
    for (const auto& [key, value] : updates) {
        if (key.empty() || key.find_first_of("=\r\n") != std::string::npos || key.find('\0') != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid INI update");
        }
    }
    const std::string ending = text.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
    std::string output;
    std::set<std::string> seen;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        const auto line = text.substr(begin, end - begin);
        std::string key, value;
        if (entry(line, key, value) && updates.contains(key)) {
            output += key + "=" + updates.at(key) + ending;
            seen.insert(key);
        } else output.append(line);
        begin = end;
    }
    for (const auto& [key, value] : updates) {
        if (seen.contains(key)) continue;
        if (!output.empty() && output.back() != '\n') output += ending;
        output += key + "=" + value + ending;
    }
    return output;
}

inline Values changed(const Values& before, const Values& after) {
    Values result;
    for (const auto& [key, value] : after) {
        const auto previous = before.find(key);
        if (previous == before.end() || previous->second != value) result[key] = value;
    }
    return result;
}

} // namespace fangcun::settings
