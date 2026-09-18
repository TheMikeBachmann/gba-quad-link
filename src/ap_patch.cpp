/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ap_patch.h"

#include <mgba-util/md5.h>
#include <mgba-util/vfs.h>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "minijson.h"

namespace gql {
namespace {

namespace fs = std::filesystem;

std::string hex(const std::uint8_t* d, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(k[d[i] >> 4]);
        out.push_back(k[d[i] & 0x0F]);
    }
    return out;
}

std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

// Pull one named member out of a zip.
//
// Enough of the format to find a file and inflate it, and no more: patch files
// are small, written by one program, and we want exactly one member out of
// them. Central directory only — the local headers lie about sizes when a data
// descriptor is used, and the central directory never does.
// When `member` is empty, every name is appended to `names` instead.
bool zip_walk(const std::string& path, const std::string& member,
              std::string* out, std::vector<std::string>* names) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 22) return false;

    // The end-of-central-directory record is last, after a comment of unknown
    // length, so it has to be searched for backwards.
    const std::streamoff window = std::min<std::streamoff>(size, 66000);
    std::string tail(static_cast<std::size_t>(window), '\0');
    f.seekg(size - window);
    f.read(tail.data(), window);
    const auto* t = reinterpret_cast<const std::uint8_t*>(tail.data());

    std::streamoff eocd = -1;
    for (std::streamoff i = window - 22; i >= 0; --i) {
        if (rd32(t + i) == 0x06054B50) { eocd = i; break; }
    }
    if (eocd < 0) return false;

    const std::uint32_t entries = rd16(t + eocd + 10);
    const std::uint32_t cd_size = rd32(t + eocd + 12);
    const std::uint32_t cd_off = rd32(t + eocd + 16);
    if (cd_off + cd_size > static_cast<std::uint32_t>(size)) return false;

    std::string cd(cd_size, '\0');
    f.seekg(cd_off);
    f.read(cd.data(), cd_size);
    const auto* c = reinterpret_cast<const std::uint8_t*>(cd.data());

    std::uint32_t at = 0;
    for (std::uint32_t n = 0; n < entries && at + 46 <= cd_size; ++n) {
        if (rd32(c + at) != 0x02014B50) return false;
        const std::uint16_t method = rd16(c + at + 10);
        const std::uint32_t csize = rd32(c + at + 20);
        const std::uint32_t usize = rd32(c + at + 24);
        const std::uint16_t name_len = rd16(c + at + 28);
        const std::uint16_t extra_len = rd16(c + at + 30);
        const std::uint16_t cmt_len = rd16(c + at + 32);
        const std::uint32_t local = rd32(c + at + 42);
        const std::string name(reinterpret_cast<const char*>(c + at + 46), name_len);
        at += 46u + name_len + extra_len + cmt_len;

        if (names) { names->push_back(name); continue; }
        if (name != member) continue;
        // Refuse anything absurd rather than allocating it.
        if (usize > (64u << 20) || csize > (64u << 20)) return false;

        std::uint8_t lh[30];
        f.seekg(local);
        f.read(reinterpret_cast<char*>(lh), sizeof lh);
        if (rd32(lh) != 0x04034B50) return false;
        const std::streamoff data = local + 30 + rd16(lh + 26) + rd16(lh + 28);

        std::string comp(csize, '\0');
        f.seekg(data);
        f.read(comp.data(), csize);
        if (!f) return false;

        if (method == 0) { *out = std::move(comp); return true; }
        if (method != 8) return false;   // only stored and deflate

        out->assign(usize, '\0');
        z_stream z{};
        // Negative window: raw deflate, no zlib header, which is what a zip
        // member is.
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
        z.next_in = reinterpret_cast<Bytef*>(comp.data());
        z.avail_in = csize;
        z.next_out = reinterpret_cast<Bytef*>(out->data());
        z.avail_out = usize;
        const int r = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        return r == Z_STREAM_END;
    }
    return names != nullptr;
}

// Words in a name, lowercased, punctuation dropped. "Pokemon - Emerald
// Version (USA, Europe).7z" and "Pokemon Emerald" then have two words in
// common, which is enough to try that file before the other nine hundred.
std::vector<std::string> words_of(const std::string& in) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int name_score(const std::vector<std::string>& want, const fs::path& p) {
    const std::vector<std::string> have = words_of(p.stem().string());
    int score = 0;
    for (const std::string& w : want)
        if (std::find(have.begin(), have.end(), w) != have.end()) ++score;
    return score;
}

// Remembering what each file hashed to, so a library is only walked once.
// Keyed on size and modification time as well as path, because a file that has
// changed underneath us must not be trusted.
struct HashCache {
    struct Entry { std::uintmax_t size; std::int64_t mtime; std::string md5; };
    std::map<std::string, Entry> entries;
    std::string path;
    bool dirty = false;

    void load(const std::string& p) {
        path = p;
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            const std::size_t a = line.find(' ');
            const std::size_t b = line.find(' ', a + 1);
            const std::size_t c = line.find(' ', b + 1);
            if (a == std::string::npos || b == std::string::npos ||
                c == std::string::npos) continue;
            Entry e;
            e.md5 = line.substr(0, a);
            e.size = std::strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
            e.mtime = std::strtoll(line.substr(b + 1, c - b - 1).c_str(), nullptr, 10);
            entries[line.substr(c + 1)] = e;
        }
    }
    void save() {
        if (!dirty || path.empty()) return;
        std::ofstream f(path, std::ios::trunc);
        for (const auto& [k, e] : entries)
            f << e.md5 << ' ' << e.size << ' ' << e.mtime << ' ' << k << '\n';
        dirty = false;
    }
};

bool looks_like_rom(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e == ".gba" || e == ".7z" || e == ".zip";
}

}  // namespace

bool zip_read(const std::string& archive, const std::string& member,
              std::string* out) {
    return zip_walk(archive, member, out, nullptr);
}

std::vector<std::string> zip_list(const std::string& archive) {
    std::vector<std::string> names;
    zip_walk(archive, {}, nullptr, &names);
    return names;
}

std::string md5_of_file(const std::string& path) {
    VFile* vf = VFileOpen(path.c_str(), O_RDONLY);
    if (!vf) return {};
    std::uint8_t digest[16];
    const bool ok = md5File(vf, digest);
    vf->close(vf);
    return ok ? hex(digest, sizeof digest) : std::string{};
}

bool read_ap_patch(const std::string& path, ApPatch* out) {
    std::string manifest;
    if (!zip_walk(path, "archipelago.json", &manifest, nullptr)) return false;

    json::Value v;
    if (!json::parse(manifest, &v) || v.type != json::Value::Type::Object)
        return false;

    out->game = v.str("game");
    out->player_name = v.str("player_name");
    out->server = v.str("server");
    // Either a string or an array of them; a world that accepts several
    // acceptable dumps lists them all.
    out->base_checksums.clear();
    if (const json::Value* bc = v.find("base_checksum")) {
        if (bc->type == json::Value::Type::String) {
            if (!bc->string.empty()) out->base_checksums.push_back(bc->string);
        } else if (bc->type == json::Value::Type::Array) {
            for (const json::Value& e : bc->array)
                if (e.type == json::Value::Type::String && !e.string.empty())
                    out->base_checksums.push_back(e.string);
        }
    }
    out->result_ending = v.str("result_file_ending", ".gba");
    out->player = static_cast<int>(v.integer("player"));
    out->valid = !out->game.empty() && !out->player_name.empty();
    return out->valid;
}

std::string find_base_rom(const std::string& dir,
                          const std::vector<std::string>& checksums,
                          const std::string& cache_dir, const std::string& game,
                          void (*progress)(const std::string&, int, int)) {
    if (dir.empty() || checksums.empty()) return {};
    std::error_code ec;

    const auto wanted = [&checksums](const std::string& h) {
        if (h.empty()) return false;
        for (const std::string& c : checksums)
            if (h == c) return true;
        return false;
    };

    // Anything already unpacked is checked first: the common case is a
    // cartridge we extracted for an earlier patch, and it costs one hash
    // rather than a walk through the whole library.
    if (!cache_dir.empty()) {
        fs::directory_iterator cached(cache_dir, ec);
        if (!ec)
            for (const fs::directory_entry& e : cached)
                if (e.is_regular_file(ec) &&
                    wanted(md5_of_file(e.path().string())))
                    return e.path().string();
    }

    std::vector<fs::path> candidates;
    fs::directory_iterator it(dir, ec);
    if (ec) return {};
    for (const fs::directory_entry& e : it)
        if (e.is_regular_file(ec) && looks_like_rom(e.path()))
            candidates.push_back(e.path());

    // Likeliest first. Walking a library alphabetically and hashing every
    // archive on the way took eighty seconds to reach Pokemon; the file whose
    // name matches the game is almost always the answer and costs one hash.
    const std::vector<std::string> want = words_of(game);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const fs::path& a, const fs::path& b) {
                         const int sa = name_score(want, a);
                         const int sb = name_score(want, b);
                         if (sa != sb) return sa > sb;
                         return a < b;
                     });

    HashCache cache;
    if (!cache_dir.empty()) {
        fs::create_directories(cache_dir, ec);
        cache.load((fs::path(cache_dir) / "checksums.txt").string());
    }

    int seen = 0;
    const int total = static_cast<int>(candidates.size());
    for (const fs::path& p : candidates) {
        ++seen;
        if (progress) progress(p.filename().string(), seen, total);

        // Anything hashed before, unchanged since, is answered from the cache.
        const std::string key = p.string();
        const std::uintmax_t sz = fs::file_size(p, ec);
        const auto wt = fs::last_write_time(p, ec);
        const auto mt = static_cast<std::int64_t>(wt.time_since_epoch().count());
        const auto known = cache.entries.find(key);
        if (known != cache.entries.end() && known->second.size == sz &&
            known->second.mtime == mt) {
            if (!wanted(known->second.md5)) continue;
            // It matched, but the bytes may be inside an archive; fall through
            // so the extraction below still happens.
        }

        // A bare cartridge can be hashed where it lies.
        if (p.extension() == ".gba" || p.extension() == ".GBA") {
            const std::string h = md5_of_file(key);
            cache.entries[key] = {sz, mt, h};
            cache.dirty = true;
            if (wanted(h)) { cache.save(); return key; }
            continue;
        }

        // An archive has to be opened. mGBA's own reader handles .7z, which is
        // what a real library is made of.
        VDir* archive = VDirOpenArchive(p.c_str());
        if (!archive) continue;
        std::string found;
        archive->rewind(archive);
        while (VDirEntry* de = archive->listNext(archive)) {
            if (de->type(de) != VFS_FILE) continue;
            VFile* vf = archive->openFile(archive, de->name(de), O_RDONLY);
            if (!vf) continue;
            std::uint8_t digest[16];
            const bool hashed = md5File(vf, digest);
            const std::string h = hashed ? hex(digest, 16) : std::string{};
            if (hashed) {
                // The archive's entry, remembered under the archive's own
                // path: one cartridge per archive is the rule in a ROM set.
                cache.entries[key] = {sz, mt, h};
                cache.dirty = true;
            }
            const bool ok = wanted(h);
            if (ok) {
                // Copy it out: Archipelago needs a real file, and a member of
                // an archive stops existing when the archive closes.
                const ssize_t n = vf->size(vf);
                if (n > 0 && !cache_dir.empty()) {
                    fs::create_directories(cache_dir, ec);
                    const fs::path dst = fs::path(cache_dir) / de->name(de);
                    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
                    vf->seek(vf, 0, SEEK_SET);
                    if (vf->read(vf, buf.data(), buf.size()) == n) {
                        std::ofstream o(dst, std::ios::binary);
                        o.write(reinterpret_cast<const char*>(buf.data()),
                                static_cast<std::streamsize>(buf.size()));
                        if (o) found = dst.string();
                    }
                }
            }
            vf->close(vf);
            if (!found.empty()) break;
        }
        archive->close(archive);
        if (!found.empty()) { cache.save(); return found; }
    }
    cache.save();
    return {};
}

}  // namespace gql
