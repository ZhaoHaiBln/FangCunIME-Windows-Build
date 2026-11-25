#include "fangcun/FangCunEngine.hpp"

#include <chrono>
#include <unordered_set>

namespace fangcun {

FangCunEngine::FangCunEngine(std::filesystem::path assetDir, std::filesystem::path dataDir)
    : dictionary_(std::move(assetDir), dataDir),
      userStore_(std::move(dataDir)) {
    workerThread_ = std::thread(&FangCunEngine::workerLoop, this);
}

FangCunEngine::~FangCunEngine() {
    close();
}

void FangCunEngine::close() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) return;

    cancelPendingQueries();
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        hasPendingTask_ = false;
    }
    workerCv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    dictionary_.close();
    userStore_.close();
}

const std::unordered_map<std::string, int32_t>& FangCunEngine::learningFrequenciesLocked() {
    if (cachedRevision_ == learningRevision_) {
        return cachedFreqs_;
    }
    uint64_t rev = learningRevision_;
    cachedFreqs_ = userStore_.allFrequencies();
    cachedRevision_ = rev;
    return cachedFreqs_;
}

std::vector<std::string> FangCunEngine::candidates(
    std::string_view input,
    InputScheme scheme,
    bool mixed,
    const std::atomic<bool>* cancelFlag) {
    if (closed_.load(std::memory_order_relaxed) ||
        (cancelFlag && cancelFlag->load(std::memory_order_relaxed))) return {};

    std::string lookupInput(input);
    if (mixed && scheme == InputScheme::NineKey) {
        std::string mapped;
        mapped.reserve(input.size());
        for (char c : input) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                mapped += PinyinCode::digits(std::string_view(&c, 1));
            } else {
                mapped.push_back(c);
            }
        }
        lookupInput = std::move(mapped);
    }

    auto found = dictionary_.lookup(lookupInput, scheme, cancelFlag);
    if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) return {};

    auto custom = userStore_.personalEntries(lookupInput, scheme);
    std::string norm = PinyinCode::normalize(lookupInput);
    std::string code = (scheme == InputScheme::Pinyin) ? PinyinCode::compact(norm) : norm;

    auto filterByExact = [&](const std::vector<DictionaryEntry>& src, bool exact) {
        std::vector<DictionaryEntry> out;
        for (const auto& e : src) {
            if ((e.code == code) == exact) out.push_back(e);
        }
        return out;
    };

    std::vector<std::string> chinese;
    std::unordered_set<std::string> seen;
    auto appendRanked = [&](const std::vector<DictionaryEntry>& bucket,
                            const std::unordered_map<std::string, int32_t>& learned) {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) return;
        for (auto& text : CandidateRanker::rank(bucket, code, learned)) {
            if (seen.insert(text).second) {
                chinese.push_back(std::move(text));
                if (chinese.size() >= 200) return;
            }
        }
    };

    {
        // Ranking reads one protected frequency snapshot. Copying the complete
        // learned-word map on every keypress grows with years of user history.
        // UserStore operations above have released their lock before this one.
        std::lock_guard<std::mutex> lock(freqMutex_);
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) return {};
        const auto& learned = learningFrequenciesLocked();
        appendRanked(filterByExact(custom, true), learned);
        if (chinese.size() < 200) appendRanked(filterByExact(found.words, true), learned);
        if (chinese.size() < 200) appendRanked(filterByExact(custom, false), learned);
        if (chinese.size() < 200) appendRanked(filterByExact(found.words, false), learned);
        if (chinese.size() < 200) appendRanked(found.sentences, learned);
    }

    if (mixed) {
        return MixedInput::candidates(input, chinese, scheme);
    }
    return chinese;
}

uint64_t FangCunEngine::queryAsync(
    std::string input,
    InputScheme scheme,
    bool mixed,
    QueryCallback callback) {
    uint64_t gen = activeGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (currentCancelFlag_) {
            currentCancelFlag_->store(true, std::memory_order_relaxed);
        }
        auto flag = std::make_shared<std::atomic<bool>>(false);
        currentCancelFlag_ = flag;
        pendingTask_ = PendingTask{gen, std::move(input), scheme, mixed, std::move(callback), std::move(flag)};
        hasPendingTask_ = true;
    }
    workerCv_.notify_one();
    return gen;
}

void FangCunEngine::cancelPendingQueries() {
    activeGeneration_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(workerMutex_);
    if (currentCancelFlag_) {
        currentCancelFlag_->store(true, std::memory_order_relaxed);
    }
    hasPendingTask_ = false;
}

void FangCunEngine::workerLoop() {
    while (true) {
        PendingTask task;
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerCv_.wait(lock, [&] {
                return closed_.load(std::memory_order_relaxed) || hasPendingTask_;
            });
            if (closed_.load(std::memory_order_relaxed)) break;
            task = std::move(pendingTask_);
            hasPendingTask_ = false;
        }

        if (task.generation != activeGeneration_.load(std::memory_order_acquire)) {
            continue;
        }

        auto result = candidates(task.input, task.scheme, task.mixed, task.cancelFlag.get());

        if (task.generation == activeGeneration_.load(std::memory_order_acquire) &&
            !task.cancelFlag->load(std::memory_order_relaxed) &&
            task.callback) {
            task.callback(task.generation, std::move(task.input), task.scheme, task.mixed, std::move(result));
        }
    }
}

void FangCunEngine::prepare(InputScheme scheme) {
    if (closed_.load(std::memory_order_relaxed)) return;
    dictionary_.prepare(scheme, nullptr);
    std::lock_guard<std::mutex> lock(freqMutex_);
    learningFrequenciesLocked();
}

void FangCunEngine::learn(std::string_view text) {
    if (closed_.load(std::memory_order_relaxed) || text.empty()) return;
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    // Use the same lock for the durable increment and snapshot adjustment. A
    // cold candidate query must not read the increment and then add it twice.
    std::lock_guard<std::mutex> lock(freqMutex_);
    if (closed_.load(std::memory_order_relaxed)) return;
    if (userStore_.incrementFrequency(text, nowMs)) {
        if (cachedRevision_ == learningRevision_) {
            cachedFreqs_[std::string(text)] += 1;
        }
    }
}

void FangCunEngine::clearLearning() {
    std::lock_guard<std::mutex> lock(freqMutex_);
    if (!userStore_.clearLearning()) return;
    ++learningRevision_;
    cachedFreqs_.clear();
    cachedRevision_ = UINT64_MAX;
}

} // namespace fangcun
