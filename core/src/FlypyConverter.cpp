#include "fangcun/FlypyConverter.hpp"

#include <algorithm>
#include <regex>
#include <utility>
#include <vector>

namespace fangcun::FlypyConverter {

namespace {

struct Rule {
    std::regex pattern;
    std::string replacement;
};

const std::vector<Rule>& rules() {
    static const std::vector<Rule> kRules = {
        {std::regex("^([jqxy])u$"), "$1v"},
        {std::regex("^([aoe])([ioun])$"), "$1$1$2"},
        {std::regex("^([aoe])(ng)?$"), "$1$1$2"},
        {std::regex("iu$"), "\xE2\x93\x86"},        // Ⓠ
        {std::regex("(.)ei$"), "$1\xE2\x93\x8C"},   // Ⓦ
        {std::regex("uan$"), "\xE2\x93\x87"},       // Ⓡ
        {std::regex("[uv]e$"), "\xE2\x93\x89"},     // Ⓣ
        {std::regex("un$"), "\xE2\x93\x8E"},        // Ⓨ
        {std::regex("^sh"), "\xE2\x93\x8A"},        // Ⓤ
        {std::regex("^ch"), "\xE2\x92\xBE"},        // Ⓘ
        {std::regex("^zh"), "\xE2\x93\x8B"},        // Ⓥ
        {std::regex("uo$"), "\xE2\x93\x84"},        // Ⓞ
        {std::regex("ie$"), "\xE2\x93\x85"},        // Ⓟ
        {std::regex("(.)i?ong$"), "$1\xE2\x93\x88"},// Ⓢ
        {std::regex("ing$|uai$"), "\xE2\x93\x80"},  // Ⓚ
        {std::regex("(.)ai$"), "$1\xE2\x92\xB9"},   // Ⓓ
        {std::regex("(.)en$"), "$1\xE2\x92\xBB"},   // Ⓕ
        {std::regex("(.)eng$"), "$1\xE2\x92\xBC"},  // Ⓖ
        {std::regex("[iu]ang$"), "\xE2\x93\x81"},   // Ⓛ
        {std::regex("(.)ang$"), "$1\xE2\x92\xBD"},  // Ⓗ
        {std::regex("ian$"), "\xE2\x93\x82"},       // Ⓜ
        {std::regex("(.)an$"), "$1\xE2\x92\xBF"},   // Ⓙ
        {std::regex("(.)ou$"), "$1\xE2\x93\x8F"},   // Ⓩ
        {std::regex("[iu]a$"), "\xE2\x93\x8D"},     // Ⓧ
        {std::regex("iao$"), "\xE2\x93\x83"},       // Ⓝ
        {std::regex("(.)ao$"), "$1\xE2\x92\xB8"},   // Ⓒ
        {std::regex("ui$"), "\xE2\x93\x8B"},        // Ⓥ
        {std::regex("in$"), "\xE2\x92\xB7"},        // Ⓑ
    };
    return kRules;
}

std::string replaceFirst(const std::string& value, const std::regex& re, const std::string& fmt) {
    return std::regex_replace(value, re, fmt, std::regex_constants::format_first_only);
}

char mapCircledLetter(std::string_view utf8) {
    // ⓆⓌⓇⓉⓎⓊⒾⓄⓅⓈⒹⒻⒼⒽⒿⓀⓁⓏⓍⒸⓋⒷⓃⓂ -> qwrtyuiopsdfghjklzxcvbnm
    static const std::pair<std::string_view, char> kMap[] = {
        {"\xE2\x93\x86", 'q'}, {"\xE2\x93\x8C", 'w'}, {"\xE2\x93\x87", 'r'},
        {"\xE2\x93\x89", 't'}, {"\xE2\x93\x8E", 'y'}, {"\xE2\x93\x8A", 'u'},
        {"\xE2\x92\xBE", 'i'}, {"\xE2\x93\x84", 'o'}, {"\xE2\x93\x85", 'p'},
        {"\xE2\x93\x88", 's'}, {"\xE2\x92\xB9", 'd'}, {"\xE2\x92\xBB", 'f'},
        {"\xE2\x92\xBC", 'g'}, {"\xE2\x92\xBD", 'h'}, {"\xE2\x92\xBF", 'j'},
        {"\xE2\x93\x80", 'k'}, {"\xE2\x93\x81", 'l'}, {"\xE2\x93\x8F", 'z'},
        {"\xE2\x93\x8D", 'x'}, {"\xE2\x92\xB8", 'c'}, {"\xE2\x93\x8B", 'v'},
        {"\xE2\x92\xB7", 'b'}, {"\xE2\x93\x83", 'n'}, {"\xE2\x93\x82", 'm'},
    };
    for (const auto& [sym, ch] : kMap) {
        if (utf8 == sym) return ch;
    }
    return 0;
}

} // namespace

std::optional<std::string> toCode(std::string_view pinyin) {
    // Trim + lowercase + 'ü' -> 'v'
    size_t start = 0;
    while (start < pinyin.size() && (pinyin[start] == ' ' || pinyin[start] == '\t')) ++start;
    size_t end = pinyin.size();
    while (end > start && (pinyin[end - 1] == ' ' || pinyin[end - 1] == '\t')) --end;
    if (start >= end) return std::nullopt;

    std::string value;
    for (size_t i = start; i < end; ++i) {
        unsigned char c = static_cast<unsigned char>(pinyin[i]);
        if (c >= 'A' && c <= 'Z') value.push_back(static_cast<char>(c - 'A' + 'a'));
        else if (c == 0xC3 && i + 1 < end && static_cast<unsigned char>(pinyin[i + 1]) == 0xBC) {
            value.push_back('v');
            ++i;
        } else {
            value.push_back(static_cast<char>(c));
        }
    }

    for (const auto& r : rules()) {
        value = replaceFirst(value, r.pattern, r.replacement);
    }

    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) {
            char lc = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
            if (lc < 'a' || lc > 'z') return std::nullopt;
            out.push_back(lc);
            ++i;
        } else if (i + 3 <= value.size()) {
            char mapped = mapCircledLetter(std::string_view(value).substr(i, 3));
            if (mapped == 0) return std::nullopt;
            out.push_back(mapped);
            i += 3;
        } else {
            return std::nullopt;
        }
    }
    return out;
}

} // namespace fangcun::FlypyConverter
