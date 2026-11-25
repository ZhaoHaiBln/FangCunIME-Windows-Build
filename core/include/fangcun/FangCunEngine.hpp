#pragma once

#include "DictionaryRepository.hpp"
#include "InputScheme.hpp"
#include "MixedInput.hpp"
#include "UserStore.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fangcun {

/**
 * Unified input engine facade.
 * - Synchronous candidates() for unit tests and direct callers.
 * - Asynchronous queryAsync() on a dedicated background worker thread with
 *   cooperative cancellation and generation verification so stale results never
 *   overwrite newer composing state or leak across focus switches.
 */
class FangCunEngine {
public:
    using QueryCallback = std::function<void(uint64_t generation, std::string input, InputScheme scheme, bool mixed, std::vector<std::string> candidates)>;

    FangCunEngine(std::filesystem::path assetDir, std::filesystem::path dataDir);
    ~FangCunEngine();

    FangCunEngine(const FangCunEngine&) = delete;
    FangCunEngine& operator=(const FangCunEngine&) = delete;

    // Synchronous lookup + ranking + optional mixed-input merge
    std::vector<std::string> candidates(
        std::string_view input,
        InputScheme scheme = InputScheme::Flypy,
        bool mixed = true,
        const std::atomic<bool>* cancelFlag = nullptr);

    // Asynchronous lookup: cancels any previous in-flight query and invokes callback
    // on the worker thread only if generation still matches the latest request.
    uint64_t queryAsync(
        std::string input,
        InputScheme scheme,
        bool mixed,
        QueryCallback callback);

    void cancelPendingQueries();

    void prepare(InputScheme scheme);
    void learn(std::string_view text);
    void clearLearning();

    UserStore& userStore() { return userStore_; }
    DictionaryRepository& dictionary() { return dictionary_; }

    void close();

private:
    struct PendingTask {
        uint64_t generation = 0;
        std::string input;
        InputScheme scheme = InputScheme::Flypy;
        bool mixed = true;
        QueryCallback callback;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
    };

    void workerLoop();
    // Callers keep freqMutex_ locked while using the returned cached snapshot.
    const std::unordered_map<std::string, int32_t>& learningFrequenciesLocked();

    DictionaryRepository dictionary_;
    UserStore userStore_;

    std::mutex freqMutex_;
    uint64_t learningRevision_ = 0;
    uint64_t cachedRevision_ = UINT64_MAX;
    std::unordered_map<std::string, int32_t> cachedFreqs_;

    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> activeGeneration_{0};

    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    bool hasPendingTask_ = false;
    PendingTask pendingTask_;
    std::shared_ptr<std::atomic<bool>> currentCancelFlag_;
    std::thread workerThread_;
};

} // namespace fangcun
