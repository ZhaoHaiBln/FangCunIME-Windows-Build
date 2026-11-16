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
} // namespace

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

KeyAction KeyHandler::resetContext() {
    ++sessionGeneration_;
    if (cancelFn_) cancelFn_();
    bool hadComposing = !composing_.empty() || !candidates_.empty();
    composing_.clear();
    candidates_.clear();
    candidatesPending_ = false;
    selectedIndex_ = 0;
    pendingBoundary_ = PendingBoundaryType::None;

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
            if (candidatesPending_ || (pageStart() + localIdx) < candidates_.size()) return true;
        }
        // Page +/- with [=] and [-]
        if ((key.ascii == '-' || key.ascii == '=') && !candidates_.empty()) {
            return true;
        }
    }

    if (chinesePunctuation_ && key.ascii != 0 && !mapChinesePunctuation(key.ascii).empty()) {
        return true;
    }

    return false;
}

KeyAction KeyHandler::onKeyDown(const KeyInput& key) {
    KeyAction action;
    if (key.ctrl || key.alt || key.win) {
        return action; // Pass through
    }
    if (englishMode_) {
        return action;
    }

    // 1. Letter / Pinyin syllable separator
    if (isComposingKey(key)) {
        if (composing_.size() >= 128) {
            // Bounded 128-code engine: ignore extra code silently or keep existing
            action.handled = true;
            return action;
        }
        pendingBoundary_ = PendingBoundaryType::None;
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
            composing_.pop_back();
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
            std::string raw = composing_;
            action = resetContext();
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
            action = resetContext();
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
                pendingDigitIndex_ = localIdx;
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
    if (chinesePunctuation_ && key.ascii != 0) {
        std::string punct = mapChinesePunctuation(key.ascii);
        if (!punct.empty()) {
            action.handled = true;
            action.commitText = true;
            action.textToCommit = std::move(punct);
            return action;
        }
    }

    return action;
}

void KeyHandler::triggerQuery(KeyAction& action) {
    candidatesPending_ = true;
    action.showCandidates = true;
    action.candidatesPending = true;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    if (asyncQuery_) {
        asyncQuery_(sessionGeneration_, composing_, scheme_, mixedInput_);
    }
}

KeyAction KeyHandler::onAsyncCandidates(
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

    if (pendingBoundary_ == PendingBoundaryType::Space) {
        pendingBoundary_ = PendingBoundaryType::None;
        if (!candidates_.empty()) {
            return commitCandidateAt(0, true);
        }
        std::string raw = composing_;
        action = resetContext();
        action.handled = true;
        action.commitText = true;
        action.textToCommit = std::move(raw);
        return action;
    }

    if (pendingBoundary_ == PendingBoundaryType::Digit) {
        size_t idx = pendingDigitIndex_;
        pendingBoundary_ = PendingBoundaryType::None;
        if (idx < candidates_.size()) {
            return commitCandidateAt(idx, false);
        }
    }

    action.handled = true;
    action.showCandidates = !composing_.empty();
    action.candidatesPending = false;
    action.candidates = candidates_;
    action.selectedIndex = selectedIndex_;
    return action;
}

KeyAction KeyHandler::selectCandidateIndex(size_t index) {
    if (candidatesPending_ || index >= candidates_.size()) {
        return {};
    }
    return commitCandidateAt(index, false);
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
        selectedIndex_ = targetPage * static_cast<int>(kPageSize);
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
    return idx / kPageSize;
}

size_t KeyHandler::pageStart() const {
    return currentPage() * kPageSize;
}

size_t KeyHandler::totalPages() const {
    if (candidates_.empty()) return 0;
    return (candidates_.size() + kPageSize - 1) / kPageSize;
}

KeyAction KeyHandler::commitCandidateAt(size_t index, bool appendEnglishSpace) {
    std::string chosen = candidates_[index];
    bool isEng = mixedInput_ && !englishMode_ && MixedInput::isEnglish(chosen);

    if (personalizedLearning_ && learnFn_) {
        learnFn_(chosen);
    }

    KeyAction action = resetContext();
    action.handled = true;
    action.commitText = true;
    action.textToCommit = chosen;
    if (appendEnglishSpace && isEng) {
        action.textToCommit.push_back(' ');
    }
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
