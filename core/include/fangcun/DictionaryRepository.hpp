#pragma once

#include "CandidateRanker.hpp"
#include "InputScheme.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace fangcun {

struct DictionaryLookup {
    std::vector<DictionaryEntry> words;
    std::vector<DictionaryEntry> sentences;

    std::vector<DictionaryEntry> all() const;
};

struct DictionaryDiagnostics {
    int32_t databaseOpens = 0;
    int32_t sqlQueries = 0;
    int32_t cacheHits = 0;
};

/**
 * Read-only SQLite dictionary repository with:
 * - Lazy / preloaded open of dict.db (Flypy, 745,701 entries) and pinyin-v1.db (Pinyin/T9, 745,702 entries)
 * - Automatic gzip decompression of pinyin-v1.dbz into dataDir if pinyin-v1.db is missing/invalid
 * - Bounded 128-code unigram word lattice sentence composition & final-syllable prefix completion
 * - Cooperative cancellation via std::atomic<bool> & sqlite3_progress_handler
 * - Bounded LRU caches for query results (64), segments (2048), and suffixes (256)
 */
class DictionaryRepository {
public:
    DictionaryRepository(std::filesystem::path assetDir, std::filesystem::path dataDir);
    ~DictionaryRepository();

    DictionaryRepository(const DictionaryRepository&) = delete;
    DictionaryRepository& operator=(const DictionaryRepository&) = delete;

    DictionaryLookup lookup(
        std::string_view input,
        InputScheme scheme,
        const std::atomic<bool>* cancelFlag = nullptr);

    std::vector<DictionaryEntry> query(
        std::string_view input,
        InputScheme scheme = InputScheme::Flypy,
        const std::atomic<bool>* cancelFlag = nullptr);

    bool prepare(InputScheme scheme, const std::atomic<bool>* cancelFlag = nullptr);
    void close();
    DictionaryDiagnostics diagnostics() const;

private:
    struct Word {
        DictionaryEntry entry;
        std::string spelling;
    };

    struct Path {
        std::string text;
        double score = 0.0;
    };

    template <typename K, typename V>
    class LruCache {
    public:
        explicit LruCache(size_t capacity) : capacity_(capacity) {}

        bool get(const K& key, V& out) {
            auto it = map_.find(key);
            if (it == map_.end()) return false;
            list_.splice(list_.begin(), list_, it->second);
            out = it->second->second;
            return true;
        }

        void put(const K& key, V value) {
            auto it = map_.find(key);
            if (it != map_.end()) {
                it->second->second = std::move(value);
                list_.splice(list_.begin(), list_, it->second);
                return;
            }
            list_.emplace_front(key, std::move(value));
            map_[key] = list_.begin();
            if (map_.size() > capacity_) {
                auto last = list_.end();
                --last;
                map_.erase(last->first);
                list_.pop_back();
            }
        }

        void clear() {
            list_.clear();
            map_.clear();
        }

    private:
        size_t capacity_;
        std::list<std::pair<K, V>> list_;
        std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
    };

    sqlite3* database(InputScheme scheme, const std::atomic<bool>* cancelFlag);
    sqlite3* openReadOnly(const std::filesystem::path& filePath);
    bool isCurrentDictionary(sqlite3* db, bool flypy, const std::atomic<bool>* cancelFlag);
    bool decompressGzip(const std::filesystem::path& src, const std::filesystem::path& dst, const std::atomic<bool>* cancelFlag);

    std::vector<DictionaryEntry> wholeWords(
        sqlite3* db,
        std::string_view input,
        std::string_view code,
        InputScheme scheme,
        const std::atomic<bool>* cancelFlag);

    std::vector<DictionaryEntry> compose(
        sqlite3* db,
        std::string_view input,
        std::string_view code,
        InputScheme scheme,
        bool hasWordPrefixes,
        const std::atomic<bool>* cancelFlag);

    std::vector<Word> segments(
        sqlite3* db,
        const std::vector<std::string>& codes,
        InputScheme scheme,
        const std::atomic<bool>* cancelFlag);

    std::vector<Word> suffixWords(
        sqlite3* db,
        std::string_view prefix,
        InputScheme scheme,
        const std::atomic<bool>* cancelFlag);

    std::vector<Word> queryWords(
        sqlite3* db,
        const std::string& sql,
        const std::vector<std::string>& args,
        InputScheme scheme,
        const std::atomic<bool>* cancelFlag);

    std::filesystem::path assetDir_;
    std::filesystem::path dataDir_;

    mutable std::mutex queryMutex_;
    bool closed_ = false;
    // Protected by queryMutex_. A canceled/failed statement is not a valid
    // negative dictionary result and must never populate any lookup cache.
    bool queryFailed_ = false;
    sqlite3* flypyDb_ = nullptr;
    sqlite3* pinyinDb_ = nullptr;

    std::atomic<int32_t> opens_{0};
    std::atomic<int32_t> sqlQueries_{0};
    std::atomic<int32_t> cacheHits_{0};

    LruCache<std::string, DictionaryLookup> resultCache_{64};
    LruCache<std::string, std::vector<Word>> segmentCache_{2048};
    LruCache<std::string, std::vector<Word>> suffixCache_{256};
};

} // namespace fangcun
