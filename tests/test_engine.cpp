#include "fangcun/CandidateRanker.hpp"
#include "fangcun/FangCunEngine.hpp"
#include "fangcun/FlypyConverter.hpp"
#include "fangcun/InputScheme.hpp"
#include "fangcun/MixedInput.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
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
