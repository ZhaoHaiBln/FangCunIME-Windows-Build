#pragma once
#include "fangcun/ResolvedSkin.hpp"
#include "fangcun/BuiltinSkinStyle.hpp"
#include <windows.h>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace fangcun::tsf {
struct SkinPixels { uint32_t width=0,height=0; std::vector<uint8_t> bgra; };
struct LoadedCustomSkin {
    skin::ResolvedSkin spec;
    bool dark=false;
    std::array<std::shared_ptr<const SkinPixels>,8> pixels{};
    const skin::Theme& theme() const {return dark?spec.dark:spec.light;}
};
struct LoadedBuiltinStyle {
    skin::BuiltinSkinStyle spec;
    bool dark = false;
    const skin::BuiltinStyleTone& tone() const { return spec.tone(dark); }
};
// Only the dedicated worker touches files/WIC. The window receives a pointer-free
// notification, then takes the latest generation's immutable CPU snapshot.
class CustomSkinCache {
public:
    static constexpr UINT ReadyMessage=WM_APP+0x321;
    CustomSkinCache();
    ~CustomSkinCache();
    void request(HWND hwnd,const std::filesystem::path& dataRoot,std::string id,std::string hash,bool dark,bool images=true);
    void requestBuiltinStyle(HWND hwnd,const std::filesystem::path& dataRoot,std::string builtin,std::string hash,bool dark);
    // Repeated queued notifications observe the same latest snapshot; taking
    // one notification must not erase a newer result before its own message.
    std::shared_ptr<const LoadedCustomSkin> takeReady();
    std::shared_ptr<const LoadedBuiltinStyle> takeBuiltinReady();
    void stop();
private:
    struct Request { HWND hwnd;std::filesystem::path root;std::string id,hash;bool dark,images;uint64_t generation;bool builtin=false; };
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Request> pending_;
    std::shared_ptr<const LoadedCustomSkin> ready_;
    std::shared_ptr<const LoadedBuiltinStyle> builtinReady_;
    uint64_t generation_=0;
    bool stopped_=false;
    std::thread worker_;
};
} // namespace fangcun::tsf
