// Host diagnostic only. This measures warmed synchronous engine queries, not
// keyboard-to-editor latency, Windows GUI responsiveness or phone performance.
// Usage: benchmark_candidates /path/to/read-only/windows/assets
#include "fangcun/FangCunEngine.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kSamples = 20;
constexpr std::size_t kWarmups = 3;

// A fixture can only remove the directory whose creation it won atomically.
// There is intentionally no command-line option for an existing data directory.
class Fixture {
public:
    Fixture() {
        std::random_device random;
        const auto parent = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            std::ostringstream name;
            name << "fangcun-candidate-benchmark-" << std::hex
                 << random() << '-' << random() << '-' << random() << '-' << attempt;
            const auto candidate = parent / name.str();
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                owned_ = true;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("Cannot create an isolated benchmark fixture");
            }
        }
        throw std::runtime_error("Cannot allocate a unique benchmark fixture");
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        if (owned_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    const std::filesystem::path& path() const { return path_; }
    void cleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) throw std::runtime_error("Cannot clean up the benchmark's own fixture");
        owned_ = false;
    }
private:
    std::filesystem::path path_;
    bool owned_ = false;
};

void sql(sqlite3* database, const char* command) {
    if (sqlite3_exec(database, command, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Isolated learning fixture transaction failed");
    }
}

void seedLearning(const std::filesystem::path& directory, int rows) {
    const auto utf8 = (directory / "user.db").u8string();
    const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(filename.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        throw std::runtime_error("Cannot open isolated learning fixture");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, &sqlite3_close);
    sql(database.get(), "CREATE TABLE user_freq(text TEXT PRIMARY KEY,count INTEGER NOT NULL,updatedAt INTEGER NOT NULL);BEGIN IMMEDIATE;");
    sqlite3_stmt* rawInsert = nullptr;
    if (sqlite3_prepare_v2(database.get(), "INSERT INTO user_freq(text,count,updatedAt) VALUES(?,?,?)", -1, &rawInsert, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Cannot prepare isolated learning fixture");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert(rawInsert, &sqlite3_finalize);
    for (int index = 0; index < rows; ++index) {
        // These deterministic synthetic names are deliberately absent from the
        // packaged dictionary, so added learning load should not change ranking.
        const auto text = "fangcun.synthetic.learning." + std::to_string(index);
        if (sqlite3_bind_text(insert.get(), 1, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(insert.get(), 2, 1 + index % 23) != SQLITE_OK ||
            sqlite3_bind_int64(insert.get(), 3, 1234567890000LL + index) != SQLITE_OK ||
            sqlite3_step(insert.get()) != SQLITE_DONE) {
            throw std::runtime_error("Cannot seed isolated learning fixture");
        }
        if (sqlite3_reset(insert.get()) != SQLITE_OK || sqlite3_clear_bindings(insert.get()) != SQLITE_OK) {
            throw std::runtime_error("Cannot reset isolated learning fixture statement");
        }
    }
    insert.reset();
    sql(database.get(), "COMMIT;");
}

std::uint64_t digest(const std::vector<std::string>& candidates) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto byte = [&](unsigned char value) { hash = (hash ^ value) * 1099511628211ULL; };
    const auto length = [&](std::size_t size) {
        const auto value = static_cast<std::uint64_t>(size);
        for (unsigned shift = 0; shift < 64; shift += 8) byte(static_cast<unsigned char>(value >> shift));
    };
    length(candidates.size());
    for (const auto& candidate : candidates) {
        length(candidate.size());
        for (const unsigned char value : candidate) byte(value);
    }
    return hash;
}

void checkCandidates(const std::vector<std::string>& candidates) {
    if (candidates.empty() || !std::any_of(candidates.begin(), candidates.end(), [](const std::string& text) {
        return std::any_of(text.begin(), text.end(), [](unsigned char value) { return value >= 0x80; });
    })) {
        throw std::runtime_error("A fixed query returned no Chinese candidates; no successful timing will be reported");
    }
}

struct Query {
    fangcun::InputScheme scheme;
    const char* schemeId;
    const char* size;
    const char* code;
};

constexpr std::array<Query, 8> kQueries{{
    {fangcun::InputScheme::Flypy, "flypy", "short", "nihc"},
    {fangcun::InputScheme::Flypy, "flypy", "short", "vsgo"},
    {fangcun::InputScheme::Flypy, "flypy", "long", "wojbtmhfkdxb"},
    {fangcun::InputScheme::Flypy, "flypy", "long", "jbtmxxwuzdjm"},
    {fangcun::InputScheme::Pinyin, "pinyin", "short", "nihao"},
    {fangcun::InputScheme::Pinyin, "pinyin", "short", "zhongguo"},
    {fangcun::InputScheme::Pinyin, "pinyin", "long", "wojintianhenkaixin"},
    {fangcun::InputScheme::Pinyin, "pinyin", "long", "jintianxiawuzaijian"},
}};

struct Measurement {
    Query query;
    int learningRows;
    std::size_t resultCount;
    std::uint64_t candidateDigest;
    std::vector<double> durations;
};

std::vector<Measurement> scenario(const std::filesystem::path& assets, int learningRows) {
    Fixture fixture;
    seedLearning(fixture.path(), learningRows);
    std::vector<Measurement> measurements;
    {
        fangcun::FangCunEngine engine(assets, fixture.path());
        engine.prepare(fangcun::InputScheme::Flypy);
        engine.prepare(fangcun::InputScheme::Pinyin);
        if (engine.userStore().allFrequencies().size() != static_cast<std::size_t>(learningRows)) {
            throw std::runtime_error("Learning fixture row count differs from the requested scenario");
        }
        for (const auto& query : kQueries) {
            std::vector<std::string> reference;
            for (std::size_t warmup = 0; warmup < kWarmups; ++warmup) {
                reference = engine.candidates(query.code, query.scheme, true);
                checkCandidates(reference);
            }
            Measurement measurement{query, learningRows, reference.size(), digest(reference), {}};
            measurement.durations.reserve(kSamples);
            for (std::size_t sample = 0; sample < kSamples; ++sample) {
                const auto start = Clock::now();
                const auto candidates = engine.candidates(query.code, query.scheme, true);
                const auto end = Clock::now();
                checkCandidates(candidates);
                if (candidates.size() != measurement.resultCount || digest(candidates) != measurement.candidateDigest) {
                    throw std::runtime_error("Candidate results changed during a fixed warmed scenario");
                }
                measurement.durations.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            }
            measurements.push_back(std::move(measurement));
        }
        engine.close();
    }
    fixture.cleanup();
    return measurements;
}

void print(const std::vector<Measurement>& measurements) {
    std::cout.imbue(std::locale::classic());
    std::cout << std::fixed << std::setprecision(6)
        << "{\"benchmark\":\"warmed synchronous FangCunEngine candidates\","
        << "\"scope\":\"isolated host diagnostic; not phone or Windows GUI input latency\","
        << "\"warmups_per_query\":" << kWarmups << ",\"samples_per_query\":" << kSamples
        << ",\"percentile_method\":\"nearest-rank\",\"mixed_input\":true,\"measurements\":[";
    bool first = true;
    for (const auto& measurement : measurements) {
        if (!first) std::cout << ',';
        first = false;
        auto sorted = measurement.durations;
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream hash;
        hash << std::hex << std::setfill('0') << std::setw(16) << measurement.candidateDigest;
        std::cout << "{\"code\":\"" << measurement.query.code << "\",\"scheme\":\"" << measurement.query.schemeId
            << "\",\"input_size\":\"" << measurement.query.size << "\",\"learning_frequency_rows\":" << measurement.learningRows
            << ",\"result_count\":" << measurement.resultCount << ",\"candidate_fnv64\":\"" << hash.str()
            << "\",\"p50_ms\":" << sorted[9] << ",\"p95_ms\":" << sorted[18]
            << ",\"min_ms\":" << sorted.front() << ",\"max_ms\":" << sorted.back() << ",\"durations_ms\":[";
        for (std::size_t index = 0; index < measurement.durations.size(); ++index) {
            if (index) std::cout << ',';
            std::cout << measurement.durations[index];
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: benchmark_candidates /path/to/read-only/windows/assets\n";
        return 2;
    }
    try {
        const auto assets = std::filesystem::canonical(std::filesystem::u8path(argv[1]));
        if (!std::filesystem::is_regular_file(assets / "dict.db") ||
            !std::filesystem::is_regular_file(assets / "pinyin-v1.dbz") ||
            !std::filesystem::is_regular_file(assets / "pinyin-v1.json")) {
            throw std::runtime_error("The specified directory is missing required packaged dictionary assets");
        }
        auto measurements = scenario(assets, 0);
        auto learned = scenario(assets, 4000);
        measurements.insert(measurements.end(), std::make_move_iterator(learned.begin()), std::make_move_iterator(learned.end()));
        print(measurements);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
