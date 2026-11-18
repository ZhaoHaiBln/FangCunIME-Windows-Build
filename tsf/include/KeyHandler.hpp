#pragma once

#include "fangcun/FangCunEngine.hpp"
#include "fangcun/InputScheme.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fangcun::tsf {

struct KeyInput {
    uint32_t vk = 0;         // Win32 Virtual-Key code (e.g. 'A'..'Z', '0'..'9', VK_SPACE=0x20, VK_BACK=0x08, VK_RETURN=0x0D, VK_ESCAPE=0x1B)
    char ascii = 0;          // Translated ASCII character if printable (0 otherwise)
    bool ctrl = false;
    bool alt = false;
    bool win = false;
    bool shift = false;
    bool capsLock = false;
};

enum class ShiftSide : uint8_t { None, Left = 1, Right = 2 };

class ShiftTapTracker {
public:
    void keyDown(ShiftSide side, bool allowTap, bool repeat);
    bool keyUp(ShiftSide side);
    void reset() { *this = {}; }

private:
    uint8_t down_ = 0;
    bool tapPending_ = false;
};

struct KeyAction {
    bool handled = false;               // true = consume key, false = pass through to host
    bool updateComposition = false;     // update TSF composing text
    std::string compositionText;        // current composing string
    bool commitText = false;            // commit text to host
    std::string textToCommit;           // UTF-8 text to commit
    bool showCandidates = false;        // show/update candidate window
    bool hideCandidates = false;        // hide candidate window
    bool candidatesPending = false;     // gray out old candidates while async query is in flight
    std::vector<std::string> candidates;
    int32_t selectedIndex = 0;
};

/**
 * Pure state machine for Windows physical keyboard input.
 * Decoupled from Win32 HWND/TSF COM so it can be unit-tested cross-platform
 * and driven by TSF ITfKeyEventSink on Windows.
 */
class KeyHandler {
public:
    using AsyncQueryFn = std::function<uint64_t(uint64_t sessionGen, std::string input, InputScheme scheme, bool mixed)>;
    using LearnFn = std::function<void(std::string_view text)>;
    using CancelQueriesFn = std::function<void()>;

    KeyHandler(AsyncQueryFn asyncQuery, LearnFn learnFn, CancelQueriesFn cancelFn);

    void setScheme(InputScheme scheme);
    InputScheme scheme() const { return scheme_; }

    void setMixedInput(bool mixed);
    bool mixedInput() const { return mixedInput_; }

    void setEnglishMode(bool english);
    bool englishMode() const { return englishMode_; }
    KeyAction toggleEnglishMode();

    void setChinesePunctuation(bool chinesePunct);
    bool chinesePunctuation() const { return chinesePunctuation_; }

    void setCapsLockBypass(bool enabled) { capsLockBypass_ = enabled; }
    bool capsLockBypass() const { return capsLockBypass_; }

    void setPageSize(size_t size) { pageSize_ = (size >= 5 && size <= 9) ? size : kPageSize; }
    size_t pageSize() const { return pageSize_; }

    void setPersonalizedLearning(bool enabled) { personalizedLearning_ = enabled; }

    // Called when focus changes, document switches, or composition is terminated externally
    KeyAction resetContext();

    // Called on WM_KEYDOWN / ITfKeyEventSink::OnTestKeyDown + OnKeyDown
    bool shouldEatKey(const KeyInput& key) const;
    KeyAction onKeyDown(const KeyInput& key);

    // Called when background engine returns candidates; verifies sessionGen + input match
    KeyAction onAsyncCandidates(uint64_t sessionGen, const std::string& input, std::vector<std::string> results);

    static constexpr size_t kPageSize = 9;

    // Select a candidate by 0-based global index (e.g. mouse click on candidate window or digit key)
    KeyAction selectCandidateIndex(size_t index);

    // Selection and page navigation
    KeyAction moveSelection(int delta);
    KeyAction movePage(int delta);

    uint64_t sessionGeneration() const { return sessionGeneration_; }
    const std::string& composing() const { return composing_; }
    const std::vector<std::string>& candidates() const { return candidates_; }
    bool candidatesPending() const { return candidatesPending_; }
    int32_t selectedIndex() const { return selectedIndex_; }
    size_t currentPage() const;
    size_t pageStart() const;
    size_t totalPages() const;

private:
    enum class PendingBoundaryType {
        None,
        Space,
        Digit,
    };

    void triggerQuery(KeyAction& action);
    KeyAction commitCandidateAt(size_t index, bool appendEnglishSpace);
    std::string mapChinesePunctuation(char ascii) const;
    bool isComposingKey(const KeyInput& key) const;

    AsyncQueryFn asyncQuery_;
    LearnFn learnFn_;
    CancelQueriesFn cancelFn_;

    InputScheme scheme_ = InputScheme::Flypy;
    bool mixedInput_ = true;
    bool englishMode_ = false;
    bool chinesePunctuation_ = true;
    bool capsLockBypass_ = true;
    size_t pageSize_ = kPageSize;
    bool personalizedLearning_ = true;

    uint64_t sessionGeneration_ = 1;
    std::string composing_;
    std::vector<std::string> candidates_;
    bool candidatesPending_ = false;
    int32_t selectedIndex_ = 0;

    PendingBoundaryType pendingBoundary_ = PendingBoundaryType::None;
    size_t pendingDigitIndex_ = 0;
};

} // namespace fangcun::tsf
