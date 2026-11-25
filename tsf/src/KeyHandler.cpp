#include "KeyHandler.hpp"

#include "fangcun/MixedInput.hpp"

#include <algorithm>

namespace fangcun::tsf {

namespace {
constexpr uint32_t kVkBack = 0x08;
constexpr uint32_t kVkReturn = 0x0D;
constexpr uint32_t kVkEscape = 0x1B;
constexpr uint32_t kVkSpace = 0x20;
constexpr uint32_t kVkLeft = 0x25;
constexpr uint32_t kVkUp = 0x26;
constexpr uint32_t kVkRight = 0x27;
constexpr uint32_t kVkDown = 0x28;
constexpr uint32_t kVkPrior = 0x21; // PageUp
constexpr uint32_t kVkNext = 0x22;  // PageDown

void eraseLastCodePoint(std::string& value) {
    if (value.empty()) return;
    auto first = value.size() - 1;
    while (first > 0 && (static_cast<unsigned char>(value[first]) & 0xC0) == 0x80) --first;
    value.erase(first);
}
} // namespace

void ShiftTapTracker::keyDown(ShiftSide side, bool allowTap, bool repeat) {
    const auto mask = static_cast<uint8_t>(side);
    if (!mask) {
        tapPending_ = false;
        return;
    }
    if ((down_ & mask) != 0) return;
    tapPending_ = down_ == 0 && allowTap && !repeat;
    down_ |= mask;
}

bool ShiftTapTracker::keyUp(ShiftSide side) {
    const auto mask = static_cast<uint8_t>(side);
    if ((down_ & mask) == 0) return false;
    down_ ^= mask;
    const bool toggle = tapPending_ && down_ == 0;
    tapPending_ = false;
    return toggle;
}

KeyHandler::KeyHandler(AsyncQueryFn asyncQuery, LearnFn learnFn, CancelQueriesFn cancelFn)
    : asyncQuery_(std::move(asyncQuery)),
      learnFn_(std::move(learnFn)),
      cancelFn_(std::move(cancelFn)) {}

void KeyHandler::setScheme(InputScheme scheme) {
    if (scheme_ != scheme) {
        scheme_ = scheme;
        resetContext();
    }
}

void KeyHandler::setMixedInput(bool mixed) {
    if (mixedInput_ != mixed) {
        mixedInput_ = mixed;
        resetContext();
    }
}

void KeyHandler::setEnglishMode(bool english) {
    if (englishMode_ != english) {
        englishMode_ = english;
        resetContext();
    }
}

void KeyHandler::setChinesePunctuation(bool chinesePunct) {
    chinesePunctuation_ = chinesePunct;
}

KeyAction KeyHandler::toggleEnglishMode() {
    beginAction();
    KeyAction action;
    if (!englishMode_ && !composing_.empty()) {
        prepareCommit();
        std::string raw = displayComposing();
        action = clearContext(true);
        action.handled = true;
        action.commitText = true;
        action.textToCommit = std::move(raw);
        action.hideCandidates = true;
    }
    englishMode_ = !englishMode_;
    return finishAction(std::move(action));
}

KeyAction KeyHandler::resetContext() {
    return clearContext(false);
}

KeyAction KeyHandler::clearContext(bool keepCommitPlan) {
    ++sessionGeneration_;
    if (cancelFn_) cancelFn_();
    bool hadComposing = !composing_.empty() || !candidates_.empty();
    composing_.clear();
    candidates_.clear();
    candidatesPending_ = false;
    selectedIndex_ = 0;
    pendingBoundary_ = PendingBoundaryType::None;
    pendingPunctuation_.clear();
    deferredKeys_.clear();
    retryCommit_ = false;
    queryAfterReplay_ = false;
    pendingCommit_.reset();
    if (!keepCommitPlan) {
        commitPlan_.reset();
        plannedLearning_.clear();
    }

    KeyAction action;
    if (hadComposing) {
        action.updateComposition = true;
        action.compositionText.clear();
        action.hideCandidates = true;
    }
    return action;
}

bool KeyHandler::isComposingKey(const KeyInput& key) const {
    if (key.ctrl || key.alt || key.win) return false;
    if (englishMode_) return false;

    if (scheme_ == InputScheme::NineKey) {
        return key.ascii >= '2' && key.ascii <= '9';
    }
    if ((key.ascii >= 'a' && key.ascii <= 'z') || (key.ascii >= 'A' && key.ascii <= 'Z')) {
        if (composing_.empty() && capsLockBypass_ && key.capsLock) {
            return false;
        }
        return true;
    }
    if (scheme_ == InputScheme::Pinyin && key.ascii == '\'' && !composing_.empty() && composing_.back() != '\'') {
        return true;
    }
    return false;
}

bool KeyHandler::shouldEatKey(const KeyInput& key) const {
    // Ctrl / Alt / Win combinations are always reserved for host app and Windows
    if (key.ctrl || key.alt || key.win) return false;

    if (englishMode_) return false;

    if (pendingBoundary_ != PendingBoundaryType::None && canDefer(key)) return true;
    if (isComposingKey(key)) return true;

    if (!composing_.empty()) {
        if (key.vk == kVkBack || key.vk == kVkEscape || key.vk == kVkReturn || key.vk == kVkSpace) {
            return true;
        }
        if (key.vk == kVkLeft || key.vk == kVkRight || key.vk == kVkUp || key.vk == kVkDown ||
            key.vk == kVkPrior || key.vk == kVkNext) {
            return true;
        }
        if (key.ascii >= '1' && key.ascii <= '9' && !key.shift) {
            size_t localIdx = static_cast<size_t>(key.ascii - '1');
            return localIdx < pageSize_ && (candidatesPending_ || (pageStart() + localIdx) < candidates_.size());
        }
        // Page +/- with [=] and [-]
        if ((key.ascii == '-' || key.ascii == '=') && !candidates_.empty()) {
            return true;
        }
        // Finish the composition before passing a literal such as @ or 0 to
        // the editor. Otherwise that literal can replace the active preedit.
        if (key.ascii >= 0x20 && key.ascii <= 0x7E) return true;
    }

    if ((chinesePunctuation_ || !composing_.empty()) && key.ascii != 0 && !mapChinesePunctuation(key.ascii).empty()) {
        return true;
    }

    return false;
}

KeyAction KeyHandler::onKeyDown(const KeyInput& key) {
    beginAction();
    return finishAction(processKeyDown(key));
}

KeyAction KeyHandler::processKeyDown(const KeyInput& key) {
    KeyAction action;
    if (key.ctrl || key.alt || key.win) {
        return action; // Pass through
    }
    if (englishMode_) {
        return action;
    }

    if (pendingBoundary_ != PendingBoundaryType::None) {
        if (retryCommit_ && key.vk == kVkReturn) {
            prepareCommit();
            auto raw = displayComposing();
            action = clearContext(true);
            action.handled = action.commitText = true;
            action.textToCommit = std::move(raw);
            return action;
        }
        if (retryCommit_ && key.vk == kVkSpace) {
            retryCommit_ = false;
            if (!candidatesPending_) return finishQueuedBoundary();
            action.handled = true;
            return action;
        }
        if (key.vk == kVkBack) {
            // Edit the uncommitted tail immediately. Queuing Backspace events
            // can otherwise eat the last tail letter and then silently lose a
            // second Backspace when replay reaches the old boundary.
            while (!deferredKeys_.empty() && deferredKeys_.back().vk == kVkReturn) deferredKeys_.pop_back();
            if (!deferredKeys_.empty()) {
                deferredKeys_.pop_back();
                action.handled = action.updateComposition = action.showCandidates = true;
                action.compositionText = displayComposing();
                action.candidatesPending = candidatesPending_;
                action.candidates = candidates_;
                action.selectedIndex = selectedIndex_;
                return action;
            }
            pendingBoundary_ = PendingBoundaryType::None;
            pendingPunctuation_.clear();
            retryCommit_ = false;
        } else if (canDefer(key)) {
            if (deferredKeys_.size() < kDeferredKeyLimit) deferredKeys_.push_back(key);
            else action.inputLimitReached = true;
            action.handled = true;
            action.updateComposition = true;
            action.compositionText = displayComposing();
            action.showCandidates = true;
            action.candidatesPending = candidatesPending_;
            action.candidates = candidates_;
            action.selectedIndex = selectedIndex_;
            return action;
        }
    }

    // 1. Letter / Pinyin syllable separator
    if (isComposingKey(key)) {
        if (composing_.size() >= 128) {
            // Bounded 128-code engine: ignore extra code silently or keep existing
            action.handled = true;
            action.inputLimitReached = true;
            return action;
        }
        pendingBoundary_ = PendingBoundaryType::None;
        pendingPunctuation_.clear();
        composing_.push_back(key.ascii);
        selectedIndex_ = 0;
        action.handled = true;
        action.updateComposition = true;
        action.compositionText = composing_;
        triggerQuery(action);
        return action;
    }

    // 2. Active composing controls
    if (!composing_.empty()) {
        if (key.vk == kVkBack) {
            pendingBoundary_ = PendingBoundaryType::None;
            eraseLastCodePoint(composing_);
            selectedIndex_ = 0;
            action.handled = true;
            action.updateComposition = true;
            action.compositionText = composing_;
            if (composing_.empty()) {
                if (cancelFn_) cancelFn_();
                candidates_.clear();
                candidatesPending_ = false;
                action.hideCandidates = true;
            } else {
                triggerQuery(action);
            }
            return action;
        }

        if (key.vk == kVkEscape) {
            action = resetContext();
            action.handled = true;
            return action;
        }

        if (key.vk == kVkReturn) {
            prepareCommit();
            std::string raw = composing_;
            action = clearContext(true);
            action.handled = true;
            action.commitText = true;
            action.textToCommit = std::move(raw);
            return action;
        }

        if (key.vk == kVkSpace) {
            action.handled = true;
            if (candidatesPending_) {
                pendingBoundary_ = PendingBoundaryType::Space;
                return action;
            }
            if (!candidates_.empty()) {
                size_t idx = (selectedIndex_ >= 0 && static_cast<size_t>(selectedIndex_) < candidates_.size())
                                 ? static_cast<size_t>(selectedIndex_)
                                 : 0;
                return commitCandidateAt(idx, true);
            }
            std::string raw = composing_;
            prepareCommit();
            action = clearContext(true);
            action.handled = true;
            action.commitText = true;
            action.textToCommit = std::move(raw);
            return action;
        }

        if (key.ascii >= '1' && key.ascii <= '9' && !key.shift) {
            size_t localIdx = static_cast<size_t>(key.ascii - '1');
            if (candidatesPending_) {
                action.handled = true;
                pendingBoundary_ = PendingBoundaryType::Digit;
                pendingDigitIndex_ = pageStart() + localIdx;
                return action;
            }
            size_t globalIdx = pageStart() + localIdx;
            if (globalIdx < candidates_.size()) {
                return commitCandidateAt(globalIdx, false);
            }
            return action; // Pass through if digit out of range
        }

        if (key.vk == kVkDown || key.vk == kVkRight) {
            return moveSelection(1);
        }
        if (key.vk == kVkUp || key.vk == kVkLeft) {
            return moveSelection(-1);
        }
        if (key.vk == kVkNext || key.ascii == '=') {
            return movePage(1);
        }
        if (key.vk == kVkPrior || key.ascii == '-') {
            return movePage(-1);
        }
    }

    // 3. Chinese punctuation (when not composing, or commit candidate + punctuation)
    if ((chinesePunctuation_ || !composing_.empty()) && key.ascii != 0) {
        std::string punct = mapChinesePunctuation(key.ascii);
        if (!punct.empty() && !chinesePunctuation_) punct.assign(1, key.ascii);
        if (!punct.empty()) {
            if (!composing_.empty()) {
                pendingPunctuation_ = std::move(punct);
                pendingBoundary_ = PendingBoundaryType::Punctuation;
                if (candidatesPending_) {
                    action.handled = true;
                    return action;
                }
                return finishQueuedBoundary();
            }
            prepareCommit();
            action.handled = true;
            action.commitText = true;
            action.textToCommit = std::move(punct);
            return action;
        }
    }

    if (!composing_.empty() && key.ascii >= 0x20 && key.ascii <= 0x7E) {
        pendingBoundary_ = PendingBoundaryType::Punctuation;
        pendingPunctuation_.assign(1, key.ascii);
        if (candidatesPending_) {
            action.handled = true;
            return action;
        }
        return finishQueuedBoundary();
    }
    return action;
}

void KeyHandler::triggerQuery(KeyAction& action) {
    candidatesPending_ = true;
    action.showCandidates = true;
    action.candidatesPending = true;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    if (replaying_) {
        queryAfterReplay_ = true;
    } else if (asyncQuery_) {
        asyncQuery_(sessionGeneration_, composing_, scheme_, mixedInput_);
    }
}

KeyAction KeyHandler::onAsyncCandidates(
    uint64_t sessionGen,
    const std::string& input,
    std::vector<std::string> results) {
    if (sessionGen != sessionGeneration_ || input != composing_) return {};
    beginAction();
    return finishAction(processAsyncCandidates(sessionGen, input, std::move(results)));
}

KeyAction KeyHandler::processAsyncCandidates(
    uint64_t sessionGen,
    const std::string& input,
    std::vector<std::string> results) {
    KeyAction action;
    if (sessionGen != sessionGeneration_ || input != composing_) {
        return action; // Stale result: discard safely
    }

    candidates_ = std::move(results);
    candidatesPending_ = false;
    selectedIndex_ = 0;

    if (pendingBoundary_ != PendingBoundaryType::None && !retryCommit_) return finishQueuedBoundary();

    action.handled = true;
    action.showCandidates = !composing_.empty();
    action.candidatesPending = false;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    return action;
}

KeyAction KeyHandler::selectCandidateIndex(size_t index) {
    beginAction();
    if (candidatesPending_ || index >= candidates_.size()) {
        return {};
    }
    if (retryCommit_ && pendingBoundary_ != PendingBoundaryType::None) {
        selectedIndex_ = static_cast<int32_t>(index);
        if (pendingBoundary_ == PendingBoundaryType::Digit) pendingDigitIndex_ = index;
        retryCommit_ = false;
        return finishAction(finishQueuedBoundary());
    }
    return finishAction(commitCandidateAt(index, false));
}

KeyAction KeyHandler::moveSelection(int delta) {
    KeyAction action;
    if (candidates_.empty()) return action;
    int count = static_cast<int>(candidates_.size());
    selectedIndex_ = std::clamp(selectedIndex_ + delta, 0, count - 1);
    action.handled = true;
    action.showCandidates = true;
    action.candidatesPending = candidatesPending_;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    return action;
}

KeyAction KeyHandler::movePage(int delta) {
    KeyAction action;
    if (candidates_.empty() || delta == 0) return action;
    int total = static_cast<int>(totalPages());
    int curPage = static_cast<int>(currentPage());
    int targetPage = std::clamp(curPage + delta, 0, std::max(0, total - 1));
    if (targetPage != curPage) {
        selectedIndex_ = targetPage * static_cast<int>(pageSize_);
    }
    action.handled = true;
    action.showCandidates = true;
    action.candidatesPending = candidatesPending_;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    return action;
}

size_t KeyHandler::currentPage() const {
    if (candidates_.empty() || selectedIndex_ <= 0) return 0;
    size_t idx = std::min(static_cast<size_t>(selectedIndex_), candidates_.size() - 1);
    return idx / pageSize_;
}

size_t KeyHandler::pageStart() const {
    return currentPage() * pageSize_;
}

size_t KeyHandler::totalPages() const {
    if (candidates_.empty()) return 0;
    return (candidates_.size() + pageSize_ - 1) / pageSize_;
}

KeyAction KeyHandler::commitCandidateAt(size_t index, bool appendEnglishSpace) {
    selectedIndex_ = static_cast<int32_t>(index);
    prepareCommit();
    std::string chosen = candidates_[index];
    bool isEng = mixedInput_ && !englishMode_ && MixedInput::isEnglish(chosen);

    if (personalizedLearning_) plannedLearning_.push_back(chosen);

    KeyAction action = clearContext(true);
    action.handled = true;
    action.commitText = true;
    action.textToCommit = chosen;
    if (appendEnglishSpace && isEng) {
        action.textToCommit.push_back(' ');
    }
    return action;
}

void KeyHandler::beginAction() {
    commitPlan_.reset();
    plannedLearning_.clear();
    // The adapter must acknowledge a synchronous edit before processing another
    // action. Never let a delayed failure restore over newer user input.
    pendingCommit_.reset();
}

void KeyHandler::prepareCommit() {
    if (commitPlan_) return;
    commitPlan_ = InputState{composing_, candidates_, candidatesPending_, selectedIndex_, englishMode_,
        pendingBoundary_, pendingDigitIndex_, pendingPunctuation_, deferredKeys_};
}

KeyAction KeyHandler::finishAction(KeyAction action) {
    if (action.commitText && commitPlan_) {
        auto receipt = nextCommitReceipt_++;
        if (receipt == 0) receipt = nextCommitReceipt_++;
        std::string literal;
        if (commitPlan_->composing.empty()) literal = action.textToCommit;
        pendingCommit_ = PendingCommit{receipt, sessionGeneration_, std::move(*commitPlan_),
            std::move(plannedLearning_), std::move(literal)};
        action.commitReceipt = receipt;
    }
    commitPlan_.reset();
    plannedLearning_.clear();
    return action;
}

KeyAction KeyHandler::acknowledgeCommit(uint64_t receipt, bool committed, bool followupApplied) {
    if (!pendingCommit_ || receipt == 0 || receipt != pendingCommit_->receipt ||
        pendingCommit_->generation != sessionGeneration_) return {};
    auto pending = std::move(*pendingCommit_);
    pendingCommit_.reset();
    if (committed) {
        if (personalizedLearning_ && learnFn_) {
            for (const auto& word : pending.learning) learnFn_(word);
        }
        if (followupApplied || composing_.empty()) return {};
        // The prefix is already permanent. Recover only the following draft;
        // restoring the original transaction here would duplicate that prefix.
        KeyAction recovery;
        recovery.handled = recovery.updateComposition = recovery.showCandidates = true;
        recovery.compositionText = displayComposing();
        recovery.candidates = candidates_;
        recovery.candidatesPending = candidatesPending_;
        recovery.selectedIndex = selectedIndex_;
        return recovery;
    }

    clearContext(false);
    composing_ = std::move(pending.previous.composing);
    candidates_ = std::move(pending.previous.candidates);
    candidatesPending_ = pending.previous.candidatesPending;
    selectedIndex_ = pending.previous.selectedIndex;
    englishMode_ = pending.previous.englishMode;
    pendingBoundary_ = pending.previous.boundary;
    pendingDigitIndex_ = pending.previous.digitIndex;
    pendingPunctuation_ = std::move(pending.previous.punctuation);
    deferredKeys_ = std::move(pending.previous.deferred);
    if (composing_.empty()) composing_ = std::move(pending.literalFallback);
    retryCommit_ = pendingBoundary_ != PendingBoundaryType::None;
    KeyAction recovery;
    recovery.handled = true;
    recovery.updateComposition = true;
    recovery.compositionText = displayComposing();
    recovery.hideCandidates = composing_.empty();
    recovery.showCandidates = !composing_.empty();
    recovery.candidates = candidates_;
    recovery.candidatesPending = candidatesPending_;
    recovery.selectedIndex = selectedIndex_;
    if (!composing_.empty() && candidatesPending_) triggerQuery(recovery);
    return recovery;
}

bool KeyHandler::canDefer(const KeyInput& key) const {
    return key.vk == kVkSpace || key.vk == kVkReturn || key.vk == kVkBack ||
        (key.ascii >= 0x20 && key.ascii <= 0x7E);
}

std::string KeyHandler::displayComposing() const {
    std::string display = composing_;
    if (pendingBoundary_ == PendingBoundaryType::Space) display.push_back(' ');
    else if (pendingBoundary_ == PendingBoundaryType::Punctuation) display += pendingPunctuation_;
    for (const auto& key : deferredKeys_) {
        if (key.vk == kVkBack) eraseLastCodePoint(display);
        else if (key.vk == kVkSpace) display.push_back(' ');
        else if (key.ascii >= 0x20 && key.ascii <= 0x7E) display.push_back(key.ascii);
    }
    return display;
}

KeyAction KeyHandler::finishQueuedBoundary() {
    prepareCommit();
    auto keys = std::move(deferredKeys_);
    deferredKeys_.clear();
    const auto boundary = pendingBoundary_;
    const auto punctuation = pendingPunctuation_;
    const auto digit = pendingDigitIndex_;
    KeyAction action;
    if (boundary == PendingBoundaryType::Digit && digit >= candidates_.size()) {
        pendingBoundary_ = PendingBoundaryType::None;
        pendingPunctuation_.clear();
        retryCommit_ = false;
        action.handled = true;
        action.showCandidates = !composing_.empty();
        action.candidates = candidates_;
        action.selectedIndex = selectedIndex_;
    } else {
        if (!candidates_.empty()) {
            const auto selected = selectedIndex_ >= 0 && static_cast<size_t>(selectedIndex_) < candidates_.size()
                ? static_cast<size_t>(selectedIndex_) : 0;
            action = commitCandidateAt(boundary == PendingBoundaryType::Digit ? digit : selected,
                boundary == PendingBoundaryType::Space);
        } else {
            auto raw = composing_;
            action = clearContext(true);
            action.handled = action.commitText = true;
            action.textToCommit = std::move(raw);
        }
        if (boundary == PendingBoundaryType::Punctuation) action.textToCommit += punctuation;
    }
    return replayDeferred(std::move(action), std::move(keys));
}

KeyAction KeyHandler::replayDeferred(KeyAction action, std::deque<KeyInput> keys) {
    if (keys.empty()) return action;
    replaying_ = true;
    queryAfterReplay_ = false;
    for (const auto& key : keys) {
        const auto next = processKeyDown(key);
        action.inputLimitReached = action.inputLimitReached || next.inputLimitReached;
        if (next.commitText) {
            action.commitText = true;
            action.textToCommit += next.textToCommit;
        } else if (!next.handled && key.ascii >= 0x20 && key.ascii <= 0x7E) {
            // A printable key which would normally pass through now belongs
            // after the completed boundary, not inside the old composition.
            prepareCommit();
            action.commitText = true;
            action.textToCommit.push_back(key.ascii);
        }
    }
    replaying_ = false;
    action.handled = true;
    action.updateComposition = true;
    action.compositionText = displayComposing();
    action.hideCandidates = composing_.empty();
    action.showCandidates = !composing_.empty();
    action.candidatesPending = candidatesPending_;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    if (queryAfterReplay_ && candidatesPending_ && !composing_.empty()) triggerQuery(action);
    queryAfterReplay_ = false;
    return action;
}

std::string KeyHandler::mapChinesePunctuation(char ascii) const {
    switch (ascii) {
        case ',': return "，";
        case '.': return "。";
        case '?': return "？";
        case '!': return "！";
        case ':': return "：";
        case ';': return "；";
        case '(': return "（";
        case ')': return "）";
        case '[': return "【";
        case ']': return "】";
        case '<': return "《";
        case '>': return "》";
        case '\\': return "、";
        case '^': return "……";
        case '_': return "——";
        default: return "";
    }
}

} // namespace fangcun::tsf
