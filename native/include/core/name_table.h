#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

/**
 * @file name_table.h
 * @brief The obfuscation map, keyed by a hash so the keys are not resident as text.
 *
 * A `std::map<std::string, std::string>` would keep every original package prefix
 * (`de.robv.android.xposed.`, `org.matrix.vector.core.`, ...) as a live std::string
 * in the injected process's heap for the whole lifetime of that process.  Those
 * keys are only ever used to *look something up*, so the text is pure liability:
 * a memory scan of the process would find it even though every read of
 * `/proc/self/maps` and every loader query is already filtered.
 *
 * So the keys are hashed on arrival and never stored.  Values are random names,
 * which are harmless to keep.  The temporary that carries a key from the parcel to
 * `Insert` is the only moment the text exists, and it is wiped before it is freed.
 */
namespace sys::rt {

class NameTable {
public:
    /**
     * @brief Hashes `key` and stores `value` under it.
     *
     * The caller's buffer is wiped on the way out.  It is a JNI-owned string in
     * practice, so wiping it is best effort - what this guarantees is that we do
     * not keep a copy.
     */
    void Insert(std::string_view key, std::string value) {
        if (key.empty()) return;
        entries_[Hash(key)] = std::move(value);
    }

    /// Looks up a prefix.  Returns nullptr when it was never inserted.
    /// The key material may come straight from a sealed table, so it is hashed
    /// in place and never materialised as a member.
    [[nodiscard]] const std::string *Find(std::string_view key) const {
        if (key.empty()) return nullptr;
        auto it = entries_.find(Hash(key));
        return it == entries_.end() ? nullptr : &it->second;
    }

    /// Same as Find, but returns an empty string when absent.
    [[nodiscard]] std::string At(std::string_view key) const {
        const std::string *found = Find(key);
        return found == nullptr ? std::string() : *found;
    }

    [[nodiscard]] bool empty() const { return entries_.empty(); }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    void Clear() { entries_.clear(); }

private:
    /// FNV-1a.  The strength of the hash is irrelevant here: an attacker who can
    /// read our heap can read the values too, and a collision would only produce a
    /// wrong replacement name, which fails loudly at class lookup.
    static std::uint64_t Hash(std::string_view key) {
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        for (unsigned char c : key) {
            hash ^= c;
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    std::unordered_map<std::uint64_t, std::string> entries_;
};

}  // namespace sys::rt
