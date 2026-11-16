#include "fangcun/UserStore.hpp"
#include "fangcun/FlypyConverter.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace fangcun {

namespace {

int32_t utf8CodePointCount(std::string_view s) {
    int32_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

const std::unordered_set<std::string>& validSyllables() {
    static const std::unordered_set<std::string> kSyllables = [] {
        const char* raw =
            "a ai an ang ao ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu "
            "ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng chi chong chou chu chua chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo "
            "da dai dan dang dao de dei den deng di dia dian diao die ding diu dong dou du duan dui dun duo "
            "e ei en eng er fa fan fang fei fen feng fo fou fu ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun guo "
            "ha hai han hang hao he hei hen heng hm hng hong hou hu hua huai huan huang hui hun huo "
            "ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun "
            "ka kai kan kang kao ke kei ken keng kong kou ku kua kuai kuan kuang kui kun kuo "
            "la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu lo long lou lu luan lun luo lv lve "
            "m ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu "
            "n na nai nan nang nao ne nei nen neng ng ni nian niang niao nie nin ning niu nong nou nu nuan nuo nv nve "
            "o ou pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu "
            "qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun "
            "ran rang rao re ren reng ri rong rou ru rua ruan rui run ruo "
            "sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan sui sun suo "
            "ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tuan tui tun tuo "
            "wa wai wan wang wei wen weng wo wu xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun "
            "ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun "
            "za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhei zhen zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo biang";
        std::unordered_set<std::string> s;
        std::istringstream iss(raw);
        std::string tok;
        while (iss >> tok) s.insert(std::move(tok));
        return s;
    }();
    return kSyllables;
}

bool isHanCodePoint(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FA1F);
}

bool extractHanCodePoints(std::string_view utf8, int32_t& outCount) {
    outCount = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            return false;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            return false;
        }
        if (!isHanCodePoint(cp)) return false;
        ++outCount;
    }
    return outCount >= 1 && outCount <= 32;
}

std::string trimAscii(std::string_view sv) {
    size_t b = 0;
    while (b < sv.size() && (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r' || sv[b] == '\n')) ++b;
    size_t e = sv.size();
    while (e > b && (sv[e - 1] == ' ' || sv[e - 1] == '\t' || sv[e - 1] == '\r' || sv[e - 1] == '\n')) --e;
    return std::string(sv.substr(b, e - b));
}

bool validateAndCompile(
    std::string_view rawText,
    std::string_view rawSpelling,
    PersonalWord& outWord,
    std::string& outFlypy,
    std::string& outPinyin,
    std::string& outDigits,
    std::string& outBoundaries,
    int32_t& outLen,
    std::string* errorMsg) {
    std::string text = trimAscii(rawText);
    if (text.empty() || text.size() > 128) {
        if (errorMsg) *errorMsg = "词语需包含1–32个汉字";
        return false;
    }
    int32_t hanCount = 0;
    if (!extractHanCodePoints(text, hanCount)) {
        if (errorMsg) *errorMsg = "词语只支持1–32个汉字，请勿包含空格、英文或标点";
        return false;
    }

    std::string norm = PinyinCode::normalize(trimAscii(rawSpelling));
    std::vector<std::string> parts;
    std::string cur;
    for (char c : norm) {
        if (c == '\'' || c == ' ' || c == '\t') {
            if (!cur.empty()) {
                parts.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(std::move(cur));

    if (static_cast<int32_t>(parts.size()) != hanCount) {
        if (errorMsg) *errorMsg = "每个汉字需填写一个完整拼音音节，用空格分隔";
        return false;
    }

    const auto& syllables = validSyllables();
    std::string flypy;
    std::string pinyin;
    std::string boundaries;
    std::string joinedSpelling;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (syllables.count(parts[i]) == 0) {
            if (errorMsg) *errorMsg = "请填写合法的完整拼音，不带声调；ü也可写成v";
            return false;
        }
        auto codeOpt = FlypyConverter::toCode(parts[i]);
        if (!codeOpt.has_value()) {
            if (errorMsg) *errorMsg = "拼音转换失败";
            return false;
        }
        if (i > 0) {
            boundaries.push_back('\'');
            joinedSpelling.push_back(' ');
        }
        flypy += *codeOpt;
        pinyin += parts[i];
        boundaries += parts[i];
        joinedSpelling += parts[i];
    }

    outWord = {std::move(text), std::move(joinedSpelling)};
    outFlypy = std::move(flypy);
    outPinyin = std::move(pinyin);
    outDigits = PinyinCode::digits(outPinyin);
    outBoundaries = std::move(boundaries);
    outLen = utf8CodePointCount(outWord.text);
    return true;
}

} // namespace

UserStore::UserStore(std::filesystem::path dataDir) : dataDir_(std::move(dataDir)) {}

UserStore::~UserStore() {
    close();
}

void UserStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
    personalLoaded_ = false;
    compiledPersonal_.clear();
}

bool UserStore::ensureOpen() {
    if (db_) return true;
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    std::filesystem::path dbPath = dataDir_ / "user.db";
    auto u8 = dbPath.u8string();
    std::string pathStr(reinterpret_cast<const char*>(u8.data()), u8.size());
    if (sqlite3_open_v2(pathStr.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
        return false;
    }
    const char* schemaSql =
        "CREATE TABLE IF NOT EXISTS user_freq ("
        "  text TEXT PRIMARY KEY,"
        "  count INTEGER NOT NULL,"
        "  updatedAt INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS personal_words ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  text TEXT NOT NULL,"
        "  spelling TEXT NOT NULL,"
        "  UNIQUE(text, spelling)"
        ");";
    char* errMsg = nullptr;
    if (sqlite3_exec(db_, schemaSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

std::unordered_map<std::string, int32_t> UserStore::allFrequencies() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, int32_t> out;
    if (!ensureOpen()) return out;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT text, count FROM user_freq", -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(stmt, 0);
        int32_t c = sqlite3_column_int(stmt, 1);
        if (t) out[reinterpret_cast<const char*>(t)] = c;
    }
    sqlite3_finalize(stmt);
    return out;
}

bool UserStore::incrementFrequency(std::string_view text, int64_t timestampMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (text.empty() || !ensureOpen()) return false;

    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }

    bool ok = true;
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO user_freq(text, count, updatedAt) VALUES(?, 0, ?)", -1, &ins, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, text.data(), static_cast<int>(text.size()), SQLITE_STATIC);
        sqlite3_bind_int64(ins, 2, timestampMs);
        if (sqlite3_step(ins) != SQLITE_DONE) ok = false;
        sqlite3_finalize(ins);
    } else {
        ok = false;
    }

    if (ok) {
        sqlite3_stmt* upd = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE user_freq SET count = count + 1, updatedAt = ? WHERE text = ?", -1, &upd, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(upd, 1, timestampMs);
            sqlite3_bind_text(upd, 2, text.data(), static_cast<int>(text.size()), SQLITE_STATIC);
            if (sqlite3_step(upd) != SQLITE_DONE) ok = false;
            sqlite3_finalize(upd);
        } else {
            ok = false;
        }
    }

    sqlite3_exec(db_, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
    return ok;
}

bool UserStore::clearLearning() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen()) return false;
    char* err = nullptr;
    bool ok = sqlite3_exec(db_, "DELETE FROM user_freq;", nullptr, nullptr, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok;
}

void UserStore::loadPersonalWordsLocked() {
    if (personalLoaded_) return;
    compiledPersonal_.clear();
    if (!ensureOpen()) return;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT text, spelling FROM personal_words ORDER BY id DESC LIMIT 500", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(stmt, 0);
            const unsigned char* s = sqlite3_column_text(stmt, 1);
            if (!t || !s) continue;
            CompiledPersonalWord item;
            if (validateAndCompile(
                    reinterpret_cast<const char*>(t),
                    reinterpret_cast<const char*>(s),
                    item.word, item.flypy, item.pinyin, item.digits, item.boundaries, item.length, nullptr)) {
                compiledPersonal_.push_back(std::move(item));
            }
        }
        sqlite3_finalize(stmt);
    }
    personalLoaded_ = true;
}

std::vector<PersonalWord> UserStore::allPersonalWords() {
    std::lock_guard<std::mutex> lock(mutex_);
    loadPersonalWordsLocked();
    std::vector<PersonalWord> out;
    out.reserve(compiledPersonal_.size());
    for (const auto& item : compiledPersonal_) out.push_back(item.word);
    return out;
}

bool UserStore::addPersonalWord(std::string_view text, std::string_view spelling, std::string* errorMsg) {
    CompiledPersonalWord item;
    if (!validateAndCompile(text, spelling, item.word, item.flypy, item.pinyin, item.digits, item.boundaries, item.length, errorMsg)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    loadPersonalWordsLocked();
    for (const auto& existing : compiledPersonal_) {
        if (existing.word.text == item.word.text && existing.word.spelling == item.word.spelling) {
            return true;
        }
    }
    if (compiledPersonal_.size() >= MAX_PERSONAL_WORDS) {
        if (errorMsg) *errorMsg = "最多保存500条，请先删除不用的词语";
        return false;
    }
    if (!ensureOpen()) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO personal_words(text, spelling) VALUES(?, ?)", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, item.word.text.data(), static_cast<int>(item.word.text.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, item.word.spelling.data(), static_cast<int>(item.word.spelling.size()), SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    if (ok) {
        compiledPersonal_.insert(compiledPersonal_.begin(), std::move(item));
    }
    return ok;
}

bool UserStore::removePersonalWord(std::string_view text, std::string_view spelling) {
    std::lock_guard<std::mutex> lock(mutex_);
    loadPersonalWordsLocked();
    if (!ensureOpen()) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM personal_words WHERE text = ? AND spelling = ?", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, text.data(), static_cast<int>(text.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, spelling.data(), static_cast<int>(spelling.size()), SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);

    if (ok) {
        compiledPersonal_.erase(
            std::remove_if(compiledPersonal_.begin(), compiledPersonal_.end(), [&](const CompiledPersonalWord& w) {
                return w.word.text == text && w.word.spelling == spelling;
            }),
            compiledPersonal_.end());
    }
    return ok;
}

std::vector<DictionaryEntry> UserStore::personalEntries(std::string_view input, InputScheme scheme) {
    std::string normalized = PinyinCode::normalize(input);
    if (!PinyinCode::valid(normalized, scheme)) return {};

    std::string code = (scheme == InputScheme::Pinyin) ? PinyinCode::compact(normalized) : normalized;
    std::optional<std::string> boundary = (scheme == InputScheme::Pinyin) ? PinyinCode::boundaryPrefix(normalized) : std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    loadPersonalWordsLocked();
    std::vector<DictionaryEntry> out;
    for (const auto& item : compiledPersonal_) {
        const std::string& fullCode =
            (scheme == InputScheme::Flypy) ? item.flypy :
            (scheme == InputScheme::Pinyin) ? item.pinyin : item.digits;
        if (fullCode.rfind(code, 0) != 0) continue;
        if (boundary.has_value() && item.boundaries.rfind(*boundary, 0) != 0) continue;
        out.push_back({fullCode, item.word.text, 1'000'000, item.length});
    }
    return out;
}

} // namespace fangcun
