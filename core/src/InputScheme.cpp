#include "fangcun/InputScheme.hpp"

#include <stdexcept>

namespace fangcun::PinyinCode {

std::string normalize(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if ((c == 'u' || c == 'U') && i + 1 < input.size() && input[i + 1] == ':') {
            out.push_back('v');
            ++i;
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c == ' ') {
            out.push_back('\'');
        } else if (c == 0xC3 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xBC) {
            // UTF-8 'ü'
            out.push_back('v');
            ++i;
        } else if (c == 0xE2 && i + 2 < input.size() &&
                   static_cast<unsigned char>(input[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(input[i + 2]) == 0x99) {
            // UTF-8 '’'
            out.push_back('\'');
            i += 2;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string compact(std::string_view input) {
    std::string norm = normalize(input);
    std::string out;
    out.reserve(norm.size());
    for (char c : norm) {
        if (c != '\'') out.push_back(c);
    }
    return out;
}

bool valid(std::string_view input, InputScheme scheme) {
    if (input.empty() || input.size() > 128) return false;
    switch (scheme) {
        case InputScheme::Flypy:
            for (char c : input) {
                if (c < 'a' || c > 'z') return false;
            }
            return true;
        case InputScheme::Pinyin:
            if (input.front() == '\'') return false;
            for (size_t i = 0; i < input.size(); ++i) {
                char c = input[i];
                if (!((c >= 'a' && c <= 'z') || c == '\'')) return false;
                if (c == '\'' && i + 1 < input.size() && input[i + 1] == '\'') return false;
            }
            return true;
        case InputScheme::NineKey:
            for (char c : input) {
                if (c < '2' || c > '9') return false;
            }
            return true;
    }
    return false;
}

std::string digits(std::string_view pinyin) {
    std::string comp = compact(pinyin);
    std::string out;
    out.reserve(comp.size());
    for (char letter : comp) {
        if (letter >= 'a' && letter <= 'c') out.push_back('2');
        else if (letter >= 'd' && letter <= 'f') out.push_back('3');
        else if (letter >= 'g' && letter <= 'i') out.push_back('4');
        else if (letter >= 'j' && letter <= 'l') out.push_back('5');
        else if (letter >= 'm' && letter <= 'o') out.push_back('6');
        else if (letter >= 'p' && letter <= 's') out.push_back('7');
        else if (letter >= 't' && letter <= 'v') out.push_back('8');
        else if (letter >= 'w' && letter <= 'z') out.push_back('9');
        else throw std::invalid_argument("Invalid pinyin letter");
    }
    return out;
}

std::optional<std::string> boundaryPrefix(std::string_view input) {
    std::string norm = normalize(input);
    size_t pos = norm.rfind('\'');
    if (pos == std::string::npos) return std::nullopt;
    return norm.substr(0, pos + 1);
}

} // namespace fangcun::PinyinCode
