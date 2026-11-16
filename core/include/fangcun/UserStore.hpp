#pragma once

#include "CandidateRanker.hpp"
#include "InputScheme.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace fangcun {

struct PersonalWord {
    std::string text;
    std::string spelling;
};

/**
 * Local-only user data store (no network, no cloud sync):
 * - user_freq SQLite table in user.db (persisted across restarts)
 * - Personal dictionary (max 500 words, compiled into Flypy, Pinyin, and NineKey codes)
 */
class UserStore {
public:
    static constexpr size_t MAX_PERSONAL_WORDS = 500;

    explicit UserStore(std::filesystem::path dataDir);
    ~UserStore();

    UserStore(const UserStore&) = delete;
    UserStore& operator=(const UserStore&) = delete;

    // User frequency learning
    std::unordered_map<std::string, int32_t> allFrequencies();
    bool incrementFrequency(std::string_view text, int64_t timestampMs);
    bool clearLearning();

    // Personal dictionary
    std::vector<PersonalWord> allPersonalWords();
    bool addPersonalWord(std::string_view text, std::string_view spelling, std::string* errorMsg = nullptr);
    bool removePersonalWord(std::string_view text, std::string_view spelling);
    std::vector<DictionaryEntry> personalEntries(std::string_view input, InputScheme scheme);

    void close();

private:
    struct CompiledPersonalWord {
        PersonalWord word;
        std::string flypy;
        std::string pinyin;
        std::string digits;
        std::string boundaries;
        int32_t length = 0;
    };

    bool ensureOpen();
    void loadPersonalWordsLocked();

    std::filesystem::path dataDir_;
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
    bool personalLoaded_ = false;
    std::vector<CompiledPersonalWord> compiledPersonal_;
};

} // namespace fangcun
