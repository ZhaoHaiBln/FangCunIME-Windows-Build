#include "fangcun/MixedInput.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace fangcun::MixedInput {

namespace {

constexpr size_t kLimit = 128;

std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else out.push_back(c);
    }
    return out;
}

bool isAsciiLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

struct Lexicon {
    std::vector<std::string> words;
    std::unordered_set<std::string> knownWords;
    std::unordered_set<std::string> knownAcronyms;
    std::unordered_map<std::string, std::vector<std::string>> wordsByDigits;
    std::unordered_set<std::string> chineseAmbiguities;
};

const Lexicon& lexicon() {
    static const Lexicon kLexicon = [] {
        Lexicon lex;
        const char* rawWords[] = {
            "the", "and", "to", "of", "a", "in", "is", "it", "you", "that", "for", "on", "with", "this", "I", "we", "he", "she", "they",
            "be", "are", "was", "were", "have", "has", "had", "do", "does", "did", "not", "can", "will", "would", "could", "should",
            "as", "at", "by", "from", "or", "an", "if", "so", "but", "my", "your", "our", "their", "his", "her", "me", "us", "them",
            "one", "two", "first", "last", "next", "today",
            "tomorrow", "yesterday", "now", "then", "soon", "later", "before", "after", "morning", "afternoon", "evening", "night",
            "hello", "hi", "hey", "thanks", "thank", "please", "sorry", "yes", "no", "okay", "welcome", "goodbye", "bye", "dear",
            "good", "great", "nice", "fine", "happy", "well", "better", "best", "new", "old", "big", "small", "long", "short",
            "more", "most", "much", "many", "some", "any", "all", "each", "other", "same", "different", "right", "wrong", "true",
            "easy", "hard", "fast", "slow", "early", "late", "hot", "cold", "warm", "cool", "free", "busy", "ready",
            "here", "there", "where", "when", "why", "what", "which", "who", "how", "about", "again", "also", "just", "only",
            "really", "very", "still", "always", "home", "work", "school",
            "office", "room", "place", "city", "family", "friend", "name", "number", "phone",
            "time", "day", "date", "world", "way", "thing", "idea",
            "water", "food", "coffee", "money", "price", "order",
            "get", "go", "come", "see", "know", "think", "want", "need", "like", "love", "help", "use", "make", "take",
            "give", "find", "tell", "ask", "say", "speak", "talk", "read", "write", "learn", "understand", "remember", "forget",
            "look", "watch", "listen", "hear", "feel", "try", "start", "stop", "wait", "stay", "leave", "keep", "bring",
            "call", "meet", "send", "reply", "share", "copy", "paste", "cut", "delete", "save", "open", "close",
            "add", "remove", "change", "choose", "select", "search", "check", "show", "hide", "move", "turn", "play", "run",
            "walk", "buy", "pay", "eat", "drink", "sleep", "live", "hope",
            "don't", "can't", "won't", "isn't", "aren't", "I'm", "you're", "we're", "they're", "it's", "that's", "let's",
            "App", "message", "text", "word", "input", "keyboard", "language", "Chinese", "English", "letter", "symbol", "emoji",
            "file", "folder", "photo", "picture", "image", "video", "music", "sound", "screen", "color", "style", "theme", "font",
            "size", "bold", "light", "dark", "green", "blue", "red", "white", "black", "gray",
            "computer", "mobile", "device", "network", "internet", "browser", "website", "page", "link", "email", "address", "account",
            "password", "code", "data", "local", "cloud", "online", "offline", "update", "version", "install", "download", "upload", "settings",
            "button", "menu", "window", "error", "test", "debug", "release", "project", "meeting", "plan", "task", "note", "list",
            "Android", "AI", "App", "ChatGPT", "OpenAI", "API", "CPU", "GPU", "USB", "WiFi", "PDF", "URL", "OK"
        };

        for (const char* w : rawWords) {
            std::string word(w);
            std::string lower = toLowerAscii(word);
            if (!lex.knownWords.insert(lower).second) continue;
            lex.words.push_back(word);
            bool allUpper = word.size() >= 2 && std::all_of(word.begin(), word.end(), [](char c) {
                return c >= 'A' && c <= 'Z';
            });
            if (allUpper) lex.knownAcronyms.insert(word);
            lex.wordsByDigits[PinyinCode::digits(word)].push_back(word);
        }

        const char* ambiguities[] = {
            "an", "in", "he", "she", "can", "you", "shi", "man", "men", "me",
            "ma", "no", "na", "ne", "nan", "ha", "hi", "hen", "hang", "ran",
            "run", "sun", "song", "sing", "long", "ling", "lin", "min", "ming",
            "bin", "ban", "bang", "pan", "pen", "pin", "ping", "tan", "tang",
            "tong", "tin", "san", "shan", "shang", "shun", "wan", "win", "ai"
        };
        for (const char* a : ambiguities) {
            lex.chineseAmbiguities.insert(a);
        }
        return lex;
    }();
    return kLexicon;
}

std::string candidateKey(std::string_view text) {
    if (isEnglish(text)) return "english:" + toLowerAscii(text);
    return "text:" + std::string(text);
}

} // namespace

bool isEnglish(std::string_view text) {
    if (text.empty()) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (isAsciiLetter(c)) continue;
        if (c == '\'' && i > 0 && i + 1 < text.size() &&
            isAsciiLetter(text[i - 1]) && isAsciiLetter(text[i + 1])) {
            continue;
        }
        return false;
    }
    return true;
}

bool preferEnglish(std::string_view input) {
    const auto& lex = lexicon();
    std::string normalized = toLowerAscii(input);
    if (lex.chineseAmbiguities.count(normalized)) {
        return lex.knownAcronyms.count(std::string(input)) > 0;
    }
    for (size_t i = 1; i < input.size(); ++i) {
        if (input[i - 1] >= 'A' && input[i - 1] <= 'Z' && input[i] >= 'A' && input[i] <= 'Z') {
            return true;
        }
    }
    return normalized.size() >= 3 && lex.knownWords.count(normalized) > 0;
}

std::vector<std::string> candidates(
    std::string_view input,
    const std::vector<std::string>& chinese,
    InputScheme scheme) {
    const auto& lex = lexicon();
    bool rawEnglish = isEnglish(input);
    std::vector<std::string> english;
    if (rawEnglish) {
        english.emplace_back(input);
    } else if (scheme == InputScheme::NineKey && !input.empty() &&
               std::all_of(input.begin(), input.end(), [](char c) { return c >= '2' && c <= '9'; })) {
        auto it = lex.wordsByDigits.find(std::string(input));
        if (it != lex.wordsByDigits.end()) english = it->second;
    }

    std::unordered_set<std::string> replacements;
    for (const auto& e : english) replacements.insert(toLowerAscii(e));

    std::vector<std::string> base;
    std::unordered_set<std::string> seen;
    for (const auto& item : chinese) {
        if (item.empty()) continue;
        if (isEnglish(item) && replacements.count(toLowerAscii(item)) > 0) continue;
        if (seen.insert(candidateKey(item)).second) {
            base.push_back(item);
            if (base.size() >= kLimit) break;
        }
    }

    if (english.empty()) return base;

    size_t insertAt = (rawEnglish && preferEnglish(input)) ? 0 : std::min<size_t>(2, base.size());
    std::vector<std::string> merged;
    merged.reserve(std::min(kLimit, base.size() + english.size()));
    std::unordered_set<std::string> mergedSeen;

    auto appendUnique = [&](const std::string& item) {
        if (merged.size() >= kLimit) return;
        if (mergedSeen.insert(candidateKey(item)).second) {
            merged.push_back(item);
        }
    };

    for (size_t i = 0; i < insertAt && i < base.size(); ++i) appendUnique(base[i]);
    for (const auto& e : english) appendUnique(e);
    for (size_t i = insertAt; i < base.size(); ++i) appendUnique(base[i]);

    return merged;
}

} // namespace fangcun::MixedInput
