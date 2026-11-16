#include "fangcun/DictionaryRepository.hpp"

#include <sqlite3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace fangcun {

namespace {

int32_t utf8CodePointCount(std::string_view s) {
    int32_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

bool isCanceled(const std::atomic<bool>* flag) {
    return flag != nullptr && flag->load(std::memory_order_relaxed);
}

int progressCallback(void* ctx) {
    auto* flag = static_cast<const std::atomic<bool>*>(ctx);
    return isCanceled(flag) ? 1 : 0;
}

const char* columnForScheme(InputScheme scheme) {
    return (scheme == InputScheme::NineKey) ? "digits" : "code";
}

std::string columnsForScheme(InputScheme scheme) {
    std::string cols = std::string(columnForScheme(scheme)) + ",text,weight,len";
    if (scheme != InputScheme::Flypy) cols += ",spelling";
    return cols;
}

char upperBoundForScheme(InputScheme scheme) {
    return (scheme == InputScheme::NineKey) ? ':' : '{';
}

size_t maximumWordCode(InputScheme scheme) {
    return (scheme == InputScheme::Flypy) ? 8 : 22;
}

double wordScore(const DictionaryEntry& entry) {
    return std::log(std::max(0.0, static_cast<double>(entry.weight)) + 1.0) - 20.0 + entry.len * 1.5;
}

bool boundaryMatches(std::string_view spelling, size_t offset, size_t consumedEnd, const std::unordered_set<size_t>& required) {
    if (required.empty()) return true;
    std::unordered_set<size_t> actual;
    size_t position = offset;
    for (char c : spelling) {
        if (c == '\'') actual.insert(position);
        else ++position;
    }
    for (size_t req : required) {
        if (req > offset && req < consumedEnd && actual.count(req) == 0) return false;
    }
    return true;
}

bool finalSyllableStarted(std::string_view entryCode, std::string_view spelling, size_t consumed, InputScheme scheme) {
    if (scheme == InputScheme::Flypy) {
        return entryCode.size() - consumed == 1;
    }
    size_t lastQuote = spelling.rfind('\'');
    if (lastQuote == std::string_view::npos) {
        return consumed > 0;
    }
    size_t prefixLetters = 0;
    for (size_t i = 0; i < lastQuote; ++i) {
        if (spelling[i] != '\'') ++prefixLetters;
    }
    return consumed > prefixLetters;
}

std::string makeCacheKey(std::string_view key, InputScheme scheme) {
    std::string out;
    out.reserve(key.size() + 2);
    out.push_back(static_cast<char>('0' + static_cast<int>(scheme)));
    out.push_back(':');
    out.append(key);
    return out;
}

} // namespace

std::vector<DictionaryEntry> DictionaryLookup::all() const {
    std::vector<DictionaryEntry> out;
    out.reserve(words.size() + sentences.size());
    std::unordered_set<std::string> seen;
    auto append = [&](const DictionaryEntry& e) {
        std::string key = e.code + '\0' + e.text;
        if (seen.insert(std::move(key)).second) {
            out.push_back(e);
        }
    };
    for (const auto& w : words) append(w);
    for (const auto& s : sentences) append(s);
    return out;
}

DictionaryRepository::DictionaryRepository(std::filesystem::path assetDir, std::filesystem::path dataDir)
    : assetDir_(std::move(assetDir)), dataDir_(std::move(dataDir)) {}

DictionaryRepository::~DictionaryRepository() {
    close();
}

void DictionaryRepository::close() {
    std::lock_guard<std::mutex> lock(queryMutex_);
    closed_ = true;
    if (flypyDb_) {
        sqlite3_close_v2(flypyDb_);
        flypyDb_ = nullptr;
    }
    if (pinyinDb_) {
        sqlite3_close_v2(pinyinDb_);
        pinyinDb_ = nullptr;
    }
    resultCache_.clear();
    segmentCache_.clear();
    suffixCache_.clear();
}

DictionaryDiagnostics DictionaryRepository::diagnostics() const {
    return {
        opens_.load(std::memory_order_relaxed),
        sqlQueries_.load(std::memory_order_relaxed),
        cacheHits_.load(std::memory_order_relaxed),
    };
}

bool DictionaryRepository::prepare(InputScheme scheme, const std::atomic<bool>* cancelFlag) {
    std::lock_guard<std::mutex> lock(queryMutex_);
    if (closed_ || isCanceled(cancelFlag)) return false;
    return database(scheme, cancelFlag) != nullptr;
}

std::vector<DictionaryEntry> DictionaryRepository::query(
    std::string_view input,
    InputScheme scheme,
    const std::atomic<bool>* cancelFlag) {
    return lookup(input, scheme, cancelFlag).all();
}

DictionaryLookup DictionaryRepository::lookup(
    std::string_view input,
    InputScheme scheme,
    const std::atomic<bool>* cancelFlag) {
    std::string normalized = PinyinCode::normalize(input);
    if (!PinyinCode::valid(normalized, scheme)) return {};

    std::lock_guard<std::mutex> lock(queryMutex_);
    if (closed_ || isCanceled(cancelFlag)) return {};

    std::string cacheKey = makeCacheKey(normalized, scheme);
    DictionaryLookup cached;
    if (resultCache_.get(cacheKey, cached)) {
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return cached;
    }

    sqlite3* db = database(scheme, cancelFlag);
    if (!db || isCanceled(cancelFlag)) return {};

    std::string code = (scheme == InputScheme::Pinyin) ? PinyinCode::compact(normalized) : normalized;
    std::vector<DictionaryEntry> words;
    if (code.size() <= maximumWordCode(scheme)) {
        words = wholeWords(db, normalized, code, scheme, cancelFlag);
    }
    if (isCanceled(cancelFlag)) return {};

    bool exactWord = std::any_of(words.begin(), words.end(), [&](const DictionaryEntry& e) {
        return e.code == code;
    });

    std::vector<DictionaryEntry> sentences;
    if (!exactWord && code.size() >= 4) {
        sentences = compose(db, normalized, code, scheme, !words.empty(), cancelFlag);
    }
    if (isCanceled(cancelFlag)) return {};

    DictionaryLookup result{std::move(words), std::move(sentences)};
    resultCache_.put(cacheKey, result);
    return result;
}

sqlite3* DictionaryRepository::openReadOnly(const std::filesystem::path& filePath) {
    sqlite3* db = nullptr;
    auto u8 = filePath.u8string();
    std::string pathStr(reinterpret_cast<const char*>(u8.data()), u8.size());
    int rc = sqlite3_open_v2(pathStr.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close_v2(db);
        return nullptr;
    }
    opens_.fetch_add(1, std::memory_order_relaxed);
    return db;
}

bool DictionaryRepository::isCurrentDictionary(sqlite3* db, bool flypy, const std::atomic<bool>* cancelFlag) {
    if (!db) return false;
    sqlite3_progress_handler(db, 1000, cancelFlag ? progressCallback : nullptr, const_cast<std::atomic<bool>*>(cancelFlag));

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key,value FROM metadata", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    std::unordered_map<std::string, std::string> values;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* k = sqlite3_column_text(stmt, 0);
        const unsigned char* v = sqlite3_column_text(stmt, 1);
        if (k && v) {
            values[reinterpret_cast<const char*>(k)] = reinterpret_cast<const char*>(v);
        }
    }
    sqlite3_finalize(stmt);

    auto itEntries = values.find("entries");
    if (itEntries == values.end()) return false;
    if (flypy) return itEntries->second == "745701";

    auto itFormat = values.find("format");
    return itEntries->second == "745702" && itFormat != values.end() && itFormat->second == "pinyin-v1";
}

bool DictionaryRepository::decompressGzip(
    const std::filesystem::path& src,
    const std::filesystem::path& dst,
    const std::atomic<bool>* cancelFlag) {
#ifdef _WIN32
    gzFile gz = gzopen_w(src.c_str(), "rb");
#else
    auto srcU8 = src.u8string();
    std::string srcStr(reinterpret_cast<const char*>(srcU8.data()), srcU8.size());
    gzFile gz = gzopen(srcStr.c_str(), "rb");
#endif
    if (!gz) return false;

    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    auto tmp = dst;
    tmp += ".tmp";

    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        gzclose(gz);
        return false;
    }

    char buf[64 * 1024];
    bool ok = true;
    while (true) {
        if (isCanceled(cancelFlag)) {
            ok = false;
            break;
        }
        int n = gzread(gz, buf, sizeof(buf));
        if (n > 0) {
            out.write(buf, n);
            if (!out) {
                ok = false;
                break;
            }
        } else if (n == 0) {
            break;
        } else {
            ok = false;
            break;
        }
    }
    out.close();
    gzclose(gz);

    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

sqlite3* DictionaryRepository::database(InputScheme scheme, const std::atomic<bool>* cancelFlag) {
    const bool flypy = (scheme == InputScheme::Flypy);
    sqlite3*& cached = flypy ? flypyDb_ : pinyinDb_;
    if (cached != nullptr) return cached;

    const char* dbName = flypy ? "dict.db" : "pinyin-v1.db";
    std::filesystem::path target = dataDir_ / "dictionary" / dbName;

    // For Flypy, we can also read directly from assetDir_/dict.db if target doesn't exist
    if (flypy && !std::filesystem::exists(target)) {
        std::filesystem::path directAsset = assetDir_ / "dict.db";
        if (std::filesystem::exists(directAsset)) {
            sqlite3* db = openReadOnly(directAsset);
            if (db && isCurrentDictionary(db, true, cancelFlag)) {
                cached = db;
                return cached;
            }
            if (db) sqlite3_close_v2(db);
        }
    }

    sqlite3* db = std::filesystem::exists(target) ? openReadOnly(target) : nullptr;
    if (db && !isCurrentDictionary(db, flypy, cancelFlag)) {
        sqlite3_close_v2(db);
        db = nullptr;
    }

    if (!db && !isCanceled(cancelFlag)) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (flypy) {
            std::filesystem::path src = assetDir_ / "dict.db";
            if (std::filesystem::exists(src)) {
                std::filesystem::path tmp = target;
                tmp += ".tmp";
                if (std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing, ec)) {
                    std::filesystem::rename(tmp, target, ec);
                    if (!ec) db = openReadOnly(target);
                }
                std::filesystem::remove(tmp, ec);
            }
        } else {
            // Check if pre-decompressed pinyin-v1.db exists in assetDir_
            std::filesystem::path plainAsset = assetDir_ / "pinyin-v1.db";
            if (std::filesystem::exists(plainAsset)) {
                db = openReadOnly(plainAsset);
                if (db && isCurrentDictionary(db, false, cancelFlag)) {
                    cached = db;
                    return cached;
                }
                if (db) {
                    sqlite3_close_v2(db);
                    db = nullptr;
                }
            }
            std::filesystem::path gzSrc = assetDir_ / "pinyin-v1.dbz";
            if (std::filesystem::exists(gzSrc) && decompressGzip(gzSrc, target, cancelFlag)) {
                db = openReadOnly(target);
            }
        }
    }

    if (db && !isCurrentDictionary(db, flypy, cancelFlag)) {
        sqlite3_close_v2(db);
        db = nullptr;
    }

    if (db) cached = db;
    return cached;
}

std::vector<DictionaryEntry> DictionaryRepository::wholeWords(
    sqlite3* db,
    std::string_view input,
    std::string_view code,
    InputScheme scheme,
    const std::atomic<bool>* cancelFlag) {
    std::string col = columnForScheme(scheme);
    std::optional<std::string> prefix = (scheme == InputScheme::Pinyin) ? PinyinCode::boundaryPrefix(input) : std::nullopt;

    std::string boundarySql;
    std::vector<std::string> boundaryArgs;
    if (prefix.has_value()) {
        if (!input.empty() && input.back() == '\'') {
            boundarySql = " AND (spelling = ? OR spelling LIKE ?)";
            boundaryArgs.push_back(prefix->substr(0, prefix->size() - 1));
            boundaryArgs.push_back(*prefix + "%");
        } else {
            boundarySql = " AND spelling LIKE ?";
            boundaryArgs.push_back(*prefix + "%");
        }
    }

    std::string cols = columnsForScheme(scheme);

    std::vector<std::string> exactArgs;
    exactArgs.emplace_back(code);
    exactArgs.insert(exactArgs.end(), boundaryArgs.begin(), boundaryArgs.end());
    std::string exactSql = "SELECT " + cols + " FROM entries WHERE " + col + " = ?" + boundarySql +
                           " ORDER BY weight DESC,len ASC LIMIT 80";
    auto exact = queryWords(db, exactSql, exactArgs, scheme, cancelFlag);

    std::vector<std::string> prefixArgs;
    prefixArgs.emplace_back(code);
    prefixArgs.push_back(std::string(code) + upperBoundForScheme(scheme));
    prefixArgs.insert(prefixArgs.end(), boundaryArgs.begin(), boundaryArgs.end());
    std::string prefixSql = "SELECT " + cols + " FROM entries WHERE " + col + " >= ? AND " + col + " < ?" +
                            boundarySql + " ORDER BY weight DESC,len ASC LIMIT 120";
    auto prefixWords = queryWords(db, prefixSql, prefixArgs, scheme, cancelFlag);

    std::vector<DictionaryEntry> out;
    out.reserve(exact.size() + prefixWords.size());
    std::unordered_set<std::string> seen;
    auto append = [&](const Word& w) {
        std::string k = w.entry.code + '\0' + w.entry.text;
        if (seen.insert(std::move(k)).second) out.push_back(w.entry);
    };
    for (const auto& w : exact) append(w);
    for (const auto& w : prefixWords) append(w);
    return out;
}

std::vector<DictionaryEntry> DictionaryRepository::compose(
    sqlite3* db,
    std::string_view input,
    std::string_view code,
    InputScheme scheme,
    bool hasWordPrefixes,
    const std::atomic<bool>* cancelFlag) {
    auto bestPaths = [](std::vector<Path> paths) {
        std::sort(paths.begin(), paths.end(), [](const Path& a, const Path& b) {
            return a.score > b.score;
        });
        std::vector<Path> out;
        std::unordered_set<std::string> seen;
        for (auto& p : paths) {
            if (seen.insert(p.text).second) {
                out.push_back(std::move(p));
                if (out.size() >= 8) break;
            }
        }
        return out;
    };

    std::vector<std::vector<Path>> paths(code.size() + 1);
    paths[0].push_back({"", 0.0});

    std::unordered_set<size_t> explicitBoundaries;
    size_t position = 0;
    for (char c : input) {
        if (c == '\'') explicitBoundaries.insert(position);
        else ++position;
    }

    const size_t maximum = maximumWordCode(scheme);
    for (size_t offset = 0; offset < code.size(); ++offset) {
        if (isCanceled(cancelFlag)) return {};
        if (paths[offset].empty()) continue;

        std::vector<std::string> codes;
        size_t maxLen = std::min(maximum, code.size() - offset);
        if (scheme == InputScheme::Flypy) {
            for (size_t len = 2; len <= maxLen; len += 2) {
                codes.emplace_back(code.substr(offset, len));
            }
        } else {
            for (size_t len = 1; len <= maxLen; ++len) {
                codes.emplace_back(code.substr(offset, len));
            }
        }

        auto segs = segments(db, codes, scheme, cancelFlag);
        for (const auto& word : segs) {
            if (isCanceled(cancelFlag)) return {};
            size_t end = offset + word.entry.code.size();
            if (end > code.size()) continue;
            if (!boundaryMatches(word.spelling, offset, end, explicitBoundaries)) continue;
            double sc = wordScore(word.entry);
            std::vector<Path> combined = paths[end];
            for (const auto& p : paths[offset]) {
                combined.push_back({p.text + word.entry.text, p.score + sc});
            }
            paths[end] = bestPaths(std::move(combined));
        }
    }

    auto fullPaths = bestPaths(paths.back());
    if (!fullPaths.empty()) {
        std::vector<DictionaryEntry> full;
        full.reserve(fullPaths.size());
        for (size_t i = 0; i < fullPaths.size(); ++i) {
            full.push_back({
                std::string(code),
                fullPaths[i].text,
                1'000'000 - static_cast<int32_t>(i),
                utf8CodePointCount(fullPaths[i].text),
            });
        }
        return full;
    }

    if (!input.empty() && input.back() == '\'') return {};

    struct Completion {
        std::string completedCode;
        Path path;
    };
    std::vector<Completion> completions;

    size_t startOffset = (code.size() > maximum) ? (code.size() - maximum) : 0;
    for (size_t offset = startOffset; offset < code.size(); ++offset) {
        if (isCanceled(cancelFlag)) return {};
        if (paths[offset].empty()) continue;
        std::string_view remaining = code.substr(offset);
        if (remaining.size() > maximum) continue;

        auto suffix = suffixWords(db, remaining, scheme, cancelFlag);
        for (const auto& word : suffix) {
            if (word.entry.code.size() <= remaining.size()) continue;
            if (!finalSyllableStarted(word.entry.code, word.spelling, remaining.size(), scheme)) continue;
            if (!boundaryMatches(word.spelling, offset, code.size(), explicitBoundaries)) continue;

            std::string completedCode = std::string(code.substr(0, offset)) + word.entry.code;
            double sc = wordScore(word.entry) - 2.0;
            for (const auto& p : paths[offset]) {
                completions.push_back({completedCode, {p.text + word.entry.text, p.score + sc}});
            }
        }
    }

    std::sort(completions.begin(), completions.end(), [](const Completion& a, const Completion& b) {
        return a.path.score > b.path.score;
    });

    const int32_t partialWeight = hasWordPrefixes ? 1 : 500'000;
    std::vector<DictionaryEntry> partial;
    std::unordered_set<std::string> seenText;
    for (const auto& comp : completions) {
        if (!seenText.insert(comp.path.text).second) continue;
        int32_t idx = static_cast<int32_t>(partial.size());
        partial.push_back({
            comp.completedCode,
            comp.path.text,
            std::max(0, partialWeight - idx),
            utf8CodePointCount(comp.path.text),
        });
        if (partial.size() >= 8) break;
    }
    return partial;
}

std::vector<DictionaryRepository::Word> DictionaryRepository::segments(
    sqlite3* db,
    const std::vector<std::string>& codes,
    InputScheme scheme,
    const std::atomic<bool>* cancelFlag) {
    std::vector<std::string> missing;
    for (const auto& c : codes) {
        std::vector<Word> dummy;
        if (!segmentCache_.get(makeCacheKey(c, scheme), dummy)) {
            missing.push_back(c);
        }
    }

    if (!missing.empty()) {
        std::string col = columnForScheme(scheme);
        std::string cols = columnsForScheme(scheme);
        std::string sql;
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) sql += " UNION ALL ";
            sql += "SELECT * FROM (SELECT " + cols + " FROM entries WHERE " + col +
                   " = ? ORDER BY weight DESC,len ASC LIMIT 6)";
        }
        auto found = queryWords(db, sql, missing, scheme, cancelFlag);
        std::unordered_map<std::string, std::vector<Word>> grouped;
        for (auto& w : found) {
            grouped[w.entry.code].push_back(std::move(w));
        }
        for (const auto& c : missing) {
            segmentCache_.put(makeCacheKey(c, scheme), grouped[c]);
        }
    }

    std::vector<Word> result;
    for (const auto& c : codes) {
        std::vector<Word> cached;
        if (segmentCache_.get(makeCacheKey(c, scheme), cached)) {
            result.insert(result.end(), cached.begin(), cached.end());
        }
    }
    return result;
}

std::vector<DictionaryRepository::Word> DictionaryRepository::suffixWords(
    sqlite3* db,
    std::string_view prefix,
    InputScheme scheme,
    const std::atomic<bool>* cancelFlag) {
    std::string key = makeCacheKey(prefix, scheme);
    std::vector<Word> cached;
    if (suffixCache_.get(key, cached)) return cached;

    std::string col = columnForScheme(scheme);
    std::string cols = columnsForScheme(scheme);
    std::string sql = "SELECT " + cols + " FROM entries WHERE " + col + " >= ? AND " + col +
                      " < ? ORDER BY weight DESC,len ASC LIMIT 40";
    std::vector<std::string> args = {
        std::string(prefix),
        std::string(prefix) + upperBoundForScheme(scheme),
    };
    auto res = queryWords(db, sql, args, scheme, cancelFlag);
    suffixCache_.put(key, res);
    return res;
}

std::vector<DictionaryRepository::Word> DictionaryRepository::queryWords(
    sqlite3* db,
    const std::string& sql,
    const std::vector<std::string>& args,
    InputScheme scheme,
    const std::atomic<bool>* cancelFlag) {
    if (isCanceled(cancelFlag)) return {};
    sqlQueries_.fetch_add(1, std::memory_order_relaxed);
    sqlite3_progress_handler(db, 500, cancelFlag ? progressCallback : nullptr, const_cast<std::atomic<bool>*>(cancelFlag));

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    for (size_t i = 0; i < args.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), args[i].data(), static_cast<int>(args[i].size()), SQLITE_STATIC);
    }

    std::vector<Word> result;
    while (true) {
        if (isCanceled(cancelFlag)) break;
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char* c = sqlite3_column_text(stmt, 0);
            const unsigned char* t = sqlite3_column_text(stmt, 1);
            int32_t w = sqlite3_column_int(stmt, 2);
            int32_t l = sqlite3_column_int(stmt, 3);
            std::string spelling;
            if (scheme != InputScheme::Flypy) {
                const unsigned char* sp = sqlite3_column_text(stmt, 4);
                if (sp) spelling = reinterpret_cast<const char*>(sp);
            }
            result.push_back({
                {
                    c ? reinterpret_cast<const char*>(c) : "",
                    t ? reinterpret_cast<const char*>(t) : "",
                    w,
                    l,
                },
                std::move(spelling),
            });
        } else {
            break;
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

} // namespace fangcun
