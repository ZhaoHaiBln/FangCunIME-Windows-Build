// Real Windows kernel checks; keep all assertions active in Release builds.
#include "AtomicFileReplace.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(expression) do { \
    if (!(expression)) throw std::runtime_error(std::string(#expression) + \
        " (Win32 error " + std::to_string(GetLastError()) + ")"); \
} while (false)

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

class Fixture {
public:
    Fixture() {
        static std::atomic<unsigned long long> next{0};
        const auto parent = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            auto path = parent / (L"fangcun-native-atomic-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) {
                path_ = std::move(path);
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::runtime_error("Cannot create isolated Windows file fixture");
        }
        throw std::runtime_error("Cannot allocate unique Windows file fixture");
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    std::filesystem::path file(const wchar_t* name) const { return path_ / name; }
private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(file.value != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
    CHECK(written == bytes.size());
    CHECK(FlushFileBuffers(file.value));
}

std::string readHandle(HANDLE handle) {
    CHECK(handle != INVALID_HANDLE_VALUE);
    const LARGE_INTEGER beginning{};
    CHECK(SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN));
    std::string result;
    char buffer[256];
    while (true) {
        DWORD count = 0;
        CHECK(ReadFile(handle, buffer, sizeof(buffer), &count, nullptr));
        if (!count) return result;
        result.append(buffer, count);
    }
}

std::string readFile(const std::filesystem::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    return readHandle(file.value);
}

void an_open_reader_keeps_the_old_snapshot_while_new_opens_see_the_new_file() {
    Fixture fixture;
    const auto destination = fixture.file(L"设置-方寸.ini");
    const auto temporary = fixture.file(L"设置-方寸.ini.owned.tmp");
    const std::string oldBytes = "scheme=flypy\nfuture=old complete snapshot\n";
    const std::string newBytes = "scheme=pinyin\nfuture=new complete snapshot\n";
    writeFile(destination, oldBytes);
    writeFile(temporary, newBytes);
    Handle reader(CreateFileW(destination.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(reader.value != INVALID_HANDLE_VALUE);
    CHECK(fangcun::tsf::replaceFlushedFile(temporary, destination));
    CHECK(readHandle(reader.value) == oldBytes);
    CHECK(readFile(destination) == newBytes);
    CHECK(!std::filesystem::exists(temporary));
}

void a_missing_destination_is_created_without_copying_or_leaving_a_temporary_file() {
    Fixture fixture;
    const auto destination = fixture.file(L"new.ini");
    const auto temporary = fixture.file(L"new.ini.owned.tmp");
    const std::string complete = "scheme=flypy\npage_size=9\n";
    writeFile(temporary, complete);
    CHECK(fangcun::tsf::replaceFlushedFile(temporary, destination));
    CHECK(readFile(destination) == complete);
    CHECK(!std::filesystem::exists(temporary));
}

void failures_keep_existing_files_and_directories_and_leave_source_owned_by_caller() {
    Fixture fixture;
    const auto original = fixture.file(L"original.ini");
    const auto temporary = fixture.file(L"original.ini.owned.tmp");
    writeFile(original, "keep original");
    writeFile(temporary, "replacement");
    CHECK(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY));
    const bool replaced = fangcun::tsf::replaceFlushedFile(temporary, original);
    // Restore even if the following assertion fails, so only our fixture remains.
    CHECK(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_NORMAL));
    CHECK(!replaced);
    CHECK(readFile(original) == "keep original");
    CHECK(readFile(temporary) == "replacement");

    const auto directory = fixture.file(L"occupied-directory");
    CHECK(std::filesystem::create_directory(directory));
    const auto marker = directory / L"original.txt";
    writeFile(marker, "directory must survive");
    CHECK(!fangcun::tsf::replaceFlushedFile(temporary, directory));
    CHECK(std::filesystem::is_directory(directory));
    CHECK(readFile(marker) == "directory must survive");
    CHECK(readFile(temporary) == "replacement");
}

} // namespace

int main() {
    try {
        an_open_reader_keeps_the_old_snapshot_while_new_opens_see_the_new_file();
        a_missing_destination_is_created_without_copying_or_leaving_a_temporary_file();
        failures_keep_existing_files_and_directories_and_leave_source_owned_by_caller();
        std::cout << "[PASS] Windows atomic replacement preserves held readers, complete snapshots and failed destinations\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
