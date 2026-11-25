#include "fangcun/CandidateRanker.hpp"
#include "fangcun/FangCunEngine.hpp"
#include "fangcun/FlypyConverter.hpp"
#include "fangcun/InputScheme.hpp"
#include "fangcun/MixedInput.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define FC_ASSERT(cond, msg)                                                      \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << msg \
                      << std::endl;                                               \
            return 1;                                                             \
        }                                                                         \
    } while (0)

bool containsText(const std::vector<std::string>& items, std::string_view target) {
    return std::find(items.begin(), items.end(), target) != items.end();
}

int runKeyHandlerTests();

int main() {
    using namespace fangcun;

    std::filesystem::path assetDir(reinterpret_cast<const char8_t*>(FANGCUN_ASSETS_DIR));
    std::filesystem::path tempDataDir = std::filesystem::temp_directory_path() / "fangcun_win_test_data";
    std::error_code ec;
    std::filesystem::remove_all(tempDataDir, ec);
    std::filesystem::create_directories(tempDataDir, ec);

    // 1. PinyinCode & FlypyConverter tests
    FC_ASSERT(PinyinCode::normalize("U:n") == "vn", "u: normalization failed");
    FC_ASSERT(PinyinCode::digits("nihao") == "64426", "digits mapping failed");
    FC_ASSERT(FlypyConverter::toCode("zhong") == std::optional<std::string>("vs"), "zhong -> vs failed");
    FC_ASSERT(FlypyConverter::toCode("shu") == std::optional<std::string>("uu"), "shu -> uu failed");
    FC_ASSERT(FlypyConverter::toCode("shuang") == std::optional<std::string>("ul"), "shuang -> ul failed");
    FC_ASSERT(FlypyConverter::toCode("pin") == std::optional<std::string>("pb"), "pin -> pb failed");

    // Transient SQLite failures must not become durable empty/partial caches.
    // These databases are isolated fixtures; the packaged dictionary is untouched.
    {
        const auto fixtureAssets = tempDataDir / "locked-assets";
        std::filesystem::create_directories(fixtureAssets);
        const auto fixtureDb = fixtureAssets / "dict.db";
        const auto fixtureU8 = fixtureDb.u8string();
        const std::string fixturePath(reinterpret_cast<const char*>(fixtureU8.data()), fixtureU8.size());
        sqlite3* raw = nullptr;
        FC_ASSERT(sqlite3_open(fixturePath.c_str(), &raw) == SQLITE_OK, "Cannot create locked dictionary fixture");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw, &sqlite3_close);
        FC_ASSERT(sqlite3_exec(writer.get(),
            "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT);"
            "INSERT INTO metadata VALUES('entries','745701');"
            "CREATE TABLE entries(code TEXT,text TEXT,weight INTEGER,len INTEGER);"
            "CREATE INDEX idx_entries_code ON entries(code);"
            "INSERT INTO entries VALUES('aa','我',900,1),('bb','爱',800,1),('cc','你',700,1);",
            nullptr, nullptr, nullptr) == SQLITE_OK, "Cannot initialize locked dictionary fixture");
        DictionaryRepository dictionary(fixtureAssets, tempDataDir / "locked-data");
        FC_ASSERT(dictionary.prepare(InputScheme::Flypy), "Fixture must be opened before the external lock");
        const auto before = dictionary.diagnostics();
        FC_ASSERT(sqlite3_exec(writer.get(), "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == SQLITE_OK,
                  "Cannot acquire the transient writer lock");
        FC_ASSERT(dictionary.lookup("aabbcc", InputScheme::Flypy).all().empty(),
                  "A busy dictionary cannot report partial success");
        FC_ASSERT(dictionary.diagnostics().sqlQueries > before.sqlQueries,
                  "The lock fixture must reach actual dictionary SQL");
        FC_ASSERT(sqlite3_exec(writer.get(), "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK,
                  "Cannot release the transient writer lock");
        auto recovered = dictionary.lookup("aabbcc", InputScheme::Flypy).all();
        FC_ASSERT(std::any_of(recovered.begin(), recovered.end(), [](const DictionaryEntry& entry) {
            return entry.text == "我爱你";
        }), "A transient lock must not poison subsequent sentence/segment lookups");
        const auto afterRecovery = dictionary.diagnostics();
        FC_ASSERT(!dictionary.lookup("aabbcc", InputScheme::Flypy).all().empty() &&
                  dictionary.diagnostics().sqlQueries == afterRecovery.sqlQueries,
                  "A successful recovery must still use the normal result cache");
    }

    {
        const auto fixtureAssets = tempDataDir / "partial-assets";
        std::filesystem::create_directories(fixtureAssets);
        const auto fixtureU8 = (fixtureAssets / "dict.db").u8string();
        const std::string fixturePath(reinterpret_cast<const char*>(fixtureU8.data()), fixtureU8.size());
        sqlite3* raw = nullptr;
        FC_ASSERT(sqlite3_open(fixturePath.c_str(), &raw) == SQLITE_OK, "Cannot create partial SQL fixture");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw, &sqlite3_close);
        FC_ASSERT(sqlite3_exec(writer.get(),
            "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT);"
            "INSERT INTO metadata VALUES('entries','745701');"
            "CREATE TABLE raw_entries(code TEXT,text TEXT,weight INTEGER,len INTEGER,broken INTEGER);"
            "CREATE INDEX idx_fixture_order ON raw_entries(code,weight DESC,len ASC);"
            "INSERT INTO raw_entries VALUES('aa','我',900,1,0),('aa','啊',800,1,1);"
            "CREATE VIEW entries AS SELECT code,"
            "CASE WHEN broken=1 THEN abs(-9223372036854775808) ELSE text END AS text,weight,len FROM raw_entries;",
            nullptr, nullptr, nullptr) == SQLITE_OK, "Cannot initialize partial SQL fixture");
        sqlite3_stmt* probe = nullptr;
        FC_ASSERT(sqlite3_prepare_v2(writer.get(),
            "SELECT code,text,weight,len FROM entries WHERE code='aa' ORDER BY weight DESC,len ASC LIMIT 80",
            -1, &probe, nullptr) == SQLITE_OK, "Cannot prepare the partial-row fixture probe");
        const int firstRow = sqlite3_step(probe);
        const int failedRow = sqlite3_step(probe);
        sqlite3_finalize(probe);
        FC_ASSERT(firstRow == SQLITE_ROW && failedRow == SQLITE_ERROR,
                  "Fixture must yield one row before its deterministic SQLite error");

        DictionaryRepository dictionary(fixtureAssets, tempDataDir / "partial-data");
        FC_ASSERT(dictionary.lookup("aa", InputScheme::Flypy).all().empty(),
                  "A statement that fails after one row must not expose partial candidates");
        FC_ASSERT(sqlite3_exec(writer.get(), "UPDATE raw_entries SET broken=0", nullptr, nullptr, nullptr) == SQLITE_OK,
                  "Cannot repair the transient SQL fixture");
        const auto recovered = dictionary.lookup("aa", InputScheme::Flypy).all();
        FC_ASSERT(recovered.size() == 2 && recovered[0].text == "我" && recovered[1].text == "啊",
                  "Partial SQL failure must not poison the exact/prefix result cache");
        dictionary.close();

        FangCunEngine engine(fixtureAssets, tempDataDir / "frequency-data");
        const auto unlearned = engine.candidates("aa", InputScheme::Flypy, false);
        FC_ASSERT(!unlearned.empty() && unlearned.front() == "我",
                  "Unlearned fixture ordering must follow dictionary weight");
        engine.learn("啊");
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto learned = engine.candidates("aa", InputScheme::Flypy, false);
            FC_ASSERT(!learned.empty() && learned.front() == "啊", "Warm frequency snapshot must reflect learning");
        }
        engine.clearLearning();
        const auto cleared = engine.candidates("aa", InputScheme::Flypy, false);
        FC_ASSERT(!cleared.empty() && cleared.front() == "我",
                  "Clearing learning must invalidate the frequency snapshot");
        engine.close();
    }

    // 2. Real Dictionary Engine tests
    {
        FangCunEngine engine(assetDir, tempDataDir);

        // Test case 1: Xiaohe Flypy "wojbtmhfkdxb" -> "我今天很开心"
        auto flypySentence = engine.candidates("wojbtmhfkdxb", InputScheme::Flypy, true);
        FC_ASSERT(!flypySentence.empty(), "Flypy sentence returned empty");
        FC_ASSERT(containsText(flypySentence, "我今天很开心"),
                  "Flypy 'wojbtmhfkdxb' did not contain '我今天很开心'");

        // Test case 2: Xiaohe exact word "ulpb" -> "双拼"
        auto flypyWord = engine.candidates("ulpb", InputScheme::Flypy, true);
        FC_ASSERT(containsText(flypyWord, "双拼"), "Flypy 'ulpb' did not contain '双拼'");

        // Test case 3: Full Pinyin "wojintianhenkaixin" -> "我今天很开心"
        auto pinyinSentence = engine.candidates("wojintianhenkaixin", InputScheme::Pinyin, true);
        FC_ASSERT(!pinyinSentence.empty(), "Pinyin sentence returned empty");
        FC_ASSERT(containsText(pinyinSentence, "我今天很开心"),
                  "Pinyin 'wojintianhenkaixin' did not contain '我今天很开心'");

        // Test case 4: NineKey "64426" -> "你好"
        auto nineKeyWord = engine.candidates("64426", InputScheme::NineKey, true);
        FC_ASSERT(containsText(nineKeyWord, "你好"), "NineKey '64426' did not contain '你好'");

        // Test case 5: MixedInput case preservation and ambiguity handling
        auto mixedAi = engine.candidates("ai", InputScheme::Pinyin, true);
        FC_ASSERT(!mixedAi.empty() && !MixedInput::isEnglish(mixedAi.front()),
                  "Lowercase 'ai' should stay Chinese-first in Pinyin");
        auto mixedHe = engine.candidates("he", InputScheme::Flypy, true);
        FC_ASSERT(!mixedHe.empty() && !MixedInput::isEnglish(mixedHe.front()),
                  "Lowercase 'he' should stay Chinese-first in Flypy");

        auto mixedAcronym = engine.candidates("AI", InputScheme::Flypy, true);
        FC_ASSERT(!mixedAcronym.empty() && mixedAcronym.front() == "AI",
                  "Acronym 'AI' should be first candidate");

        // Test case 6: Learning persistence and clearLearning
        engine.learn("方寸");
        auto freqs = engine.userStore().allFrequencies();
        FC_ASSERT(freqs["方寸"] == 1, "Learning frequency not persisted");
        engine.clearLearning();
        freqs = engine.userStore().allFrequencies();
        FC_ASSERT(freqs.empty(), "clearLearning did not clear user_freq");

        // Test case 7: Personal dictionary compilation across all 3 schemes
        std::string errMsg;
        FC_ASSERT(engine.userStore().addPersonalWord("方寸测试", "fang cun ce shi", &errMsg),
                  "Failed to add personal word: " + errMsg);
        auto pFlypy = engine.candidates("fhcyceui", InputScheme::Flypy, false);
        FC_ASSERT(containsText(pFlypy, "方寸测试"), "Personal word not found in Flypy");
        auto pPinyin = engine.candidates("fangcunceshi", InputScheme::Pinyin, false);
        FC_ASSERT(containsText(pPinyin, "方寸测试"), "Personal word not found in Pinyin");
        auto pNine = engine.candidates("326428623744", InputScheme::NineKey, false);
        FC_ASSERT(containsText(pNine, "方寸测试"), "Personal word not found in NineKey");

        // Longer typing remains bounded and returns a complete Chinese phrase,
        // including after an explicitly canceled request for the same code.
        std::string longFlypy;
        for (int i = 0; i < 17; ++i) longFlypy += "wo";
        std::atomic<bool> canceled{true};
        FC_ASSERT(engine.candidates(longFlypy, InputScheme::Flypy, false, &canceled).empty(),
                  "Canceled long input must not return results");
        const auto longCandidates = engine.candidates(longFlypy, InputScheme::Flypy, false);
        FC_ASSERT(!longCandidates.empty(), "A following uncanceled long query must recover candidates");
        for (const auto& text : longCandidates) {
            const auto points = std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xC0) != 0x80; });
            FC_ASSERT(points == 17, "Long Flypy composition must retain every syllable without truncating the tail");
        }

        // Test case 8: Async query with generation check
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::vector<std::string> asyncResult;
        engine.queryAsync("wojbtmhfkdxb", InputScheme::Flypy, true,
                          [&](uint64_t, std::string, InputScheme, bool, std::vector<std::string> res) {
                              std::lock_guard<std::mutex> lk(m);
                              asyncResult = std::move(res);
                              done = true;
                              cv.notify_one();
                          });
        {
            std::unique_lock<std::mutex> lk(m);
            FC_ASSERT(cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; }),
                      "Async query timed out");
        }
        FC_ASSERT(containsText(asyncResult, "我今天很开心"),
                  "Async query result missing '我今天很开心'");

        engine.close();
    }

    if (runKeyHandlerTests() != 0) {
        return 1;
    }

    std::filesystem::remove_all(tempDataDir, ec);
    std::cout << "[PASS] All FangCun core engine and keyhandler tests passed!" << std::endl;
    return 0;
}
