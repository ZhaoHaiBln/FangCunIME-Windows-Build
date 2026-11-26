#include "fangcun/BuiltinSkinStyle.hpp"
#include <iostream>

using namespace fangcun::skin;
#define CHECK(condition, label) do { if (!(condition)) { std::cerr << "[FAIL] " << label << " at " << __LINE__ << '\n'; return 1; } } while (false)

int main() {
    const std::string hash(64, 'a');
    const auto fixture = [&](std::string_view id = "typewriter") {
        return "schema_version=1\nbuiltin=" + std::string(id) + "\nhash=" + hash + "\n";
    };
    for (const auto id : {"wood_type", "typewriter", "terminal", "brutal"}) {
        const auto value = parseBuiltinSkinStyleIni(fixture(id), id, hash);
        CHECK(value && !value->fontFamily && !value->pressOffsetDp, "Default builtin remains unchanged");
        for (const auto& color : value->light.colors) CHECK(!color, "No invented light defaults");
        for (const auto& color : value->dark.colors) CHECK(!color, "No invented dark defaults");
    }
    const auto edited = parseBuiltinSkinStyleIni(fixture() +
        "font_family=mono\npress_offset_dp=1.5\ndark_key_text=#abcdef\nlight_accent=#102030\n"
        "dark_glow_0=#111111\ndark_glow_1=#222222\ndark_glow_2=#333333\ndark_glow_3=#444444\n", "typewriter", hash);
    CHECK(edited && edited->fontFamily == "mono" && *edited->pressOffsetDp == 1.5f, "Global font and press edit");
    CHECK(edited->dark.colors[size_t(ColorRole::KeyText)]->r == 0xab, "Dark partial color read");
    CHECK(!edited->light.colors[size_t(ColorRole::KeyText)] && !edited->light.glow, "Modes stay independent");
    Theme base;
    for (auto& color : base.colors) color = fangcun::ColorArgb::fromHex(0xfffedcba);
    for (auto& color : base.glow) color = fangcun::ColorArgb::fromHex(0xff999999);
    base.images[0] = ImageSpec{"original.png", ImageScale::Tile, {}};
    const auto light = overlayBuiltinTone(edited->light, base);
    CHECK(light.colors[size_t(ColorRole::Accent)].r == 0x10, "Only selected role changes");
    CHECK(light.colors[size_t(ColorRole::Text)].r == 0xfe && light.glow[0].r == 0x99, "Base palette and glow inherited");
    CHECK(light.images[0] && light.images[0]->file == "original.png", "Material identity is not replaced");
    const auto dark = overlayBuiltinTone(edited->dark, base);
    CHECK(dark.glow[3].r == 0x44, "Four glow colors preserved");

    for (const auto value : {"0", "2", "1e-100", "5e-324"})
        CHECK(parseBuiltinSkinStyleIni(fixture() + "press_offset_dp=" + value + "\n", "typewriter", hash), "Valid press range");
    for (const auto value : {"-0.1", "2.00000001", "nan", "inf", "true", "1 trailing"})
        CHECK(!parseBuiltinSkinStyleIni(fixture() + "press_offset_dp=" + value + "\n", "typewriter", hash), "Reject invalid original precision/range");
    for (const auto suffix : {
        "schema_version=1\n", "font_family=other\n", "font_family=\n", "unknown=x\n",
        "light_key_text=red\n", "light_key_text=#12ffff00\n", "light_key_text=#000000\nlight_key_text=#ffffff\n",
        "dark_glow_0=#000000\n", "dark_glow_4=#000000\n", "light_image_key_normal=http://example.test/a.png\n",
        "light_surface=#FFFFFF\x01\n", "light_surface=#FFFFFF \n", "light_surface =#ffffff\n"})
        CHECK(!parseBuiltinSkinStyleIni(fixture() + suffix, "typewriter", hash), "Malformed or foreign metadata rejected");
    CHECK(!parseBuiltinSkinStyleIni(fixture(), "terminal", hash), "Wrong builtin cannot apply");
    CHECK(!parseBuiltinSkinStyleIni(fixture(), "typewriter", std::string(64, 'b')), "Stale hash cannot apply");
    CHECK(!parseBuiltinSkinStyleIni(fixture("../escape"), "../escape", hash), "Path-like builtin rejected");
    CHECK(!parseBuiltinSkinStyleIni(fixture() + std::string(16384, ' '), "typewriter", hash), "Metadata budget bounded");
    const auto crlf = "schema_version=1\r\nbuiltin=typewriter\r\nhash=" + hash + "\r\n";
    CHECK(parseBuiltinSkinStyleIni(crlf, "typewriter", hash), "Windows line endings accepted");
    std::cout << "[PASS] builtin style inheritance, mode isolation and strict partial protocol\n";
}
