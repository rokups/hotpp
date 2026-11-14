// The fine interface data of a unit and the stale rule of an importer (doc/module-level.md).
// The translator fills IfaceInfo and UseInfo. The loader, the host and the launcher use
// importer_is_stale to find the importers that must compile again after a unit changes.
// The header has no dependency on the translator. It is AOT safe: no dynamic registration.
#pragma once

#include "hot/runtime/hashing.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace hot
{

// One member name of an interface. "name" is the hash of the bare name (a constructor has the
// class name). "decl" is the hash of all interface lines with that name, in source order.
struct IfaceMember
{
    uint64_t name = 0;
    uint64_t decl = 0;
};

inline bool operator==(const IfaceMember& a, const IfaceMember& b)
{
    return a.name == b.name && a.decl == b.decl;
}

// The fine interface data of a unit.
//  - hash: the iface_hash of the unit (hotc::TranslateResult::iface_hash).
//  - shape: the hash of the parts that each importer copies or that set a layout: the namespace
//    lines, the aliases, the enums, the constants, each struct in full, each class header, the
//    virtual, override, abstract and operator members of a class, the script interfaces and the stubs.
//  - members: the other members of the classes and the public free functions and variables. The
//    list is sorted by name and has one entry for each name.
//  - names: the sorted identifier hashes of the interface text.
//  - known: false when the data is not available (an old AOT table, an old cache entry). Then
//    importer_is_stale gives "stale" for each importer (rule 2).
struct IfaceInfo
{
    bool known = false;
    uint64_t hash = 0;
    uint64_t shape = 0;
    std::vector<IfaceMember> members;
    std::vector<uint64_t> names;
};

inline bool operator==(const IfaceInfo& a, const IfaceInfo& b)
{
    return a.known == b.known && a.hash == b.hash && a.shape == b.shape && a.members == b.members && a.names == b.names;
}

inline bool operator!=(const IfaceInfo& a, const IfaceInfo& b)
{
    return !(a == b);
}

// The names that a unit uses.
//  - names: the sorted identifier hashes of the own script text. Comments and strings are in it.
//  - strong: the units that the importer needs in full. A class of the importer has a parent
//    chain into such a unit, so the layout and the slots of the parent are part of the importer.
//  - known: false when the data is not available. Then importer_is_stale gives "stale" (rule 2).
struct UseInfo
{
    bool known = false;
    std::vector<uint64_t> names;
    std::vector<std::string> strong;
};

inline bool operator==(const UseInfo& a, const UseInfo& b)
{
    return a.known == b.known && a.names == b.names && a.strong == b.strong;
}

inline bool operator!=(const UseInfo& a, const UseInfo& b)
{
    return !(a == b);
}

// The hash of one identifier.
inline uint64_t iface_name_hash(std::string_view name)
{
    return hash_text(name);
}

// The sorted identifier hashes of a text, each one time. An identifier is a letter or "_",
// then letters, digits or "_". The function does not know comments or strings: a word in
// them is a name too. A name that a macro makes by token paste is not found.
inline std::vector<uint64_t> iface_name_set(std::string_view text)
{
    auto head = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    auto tail = [&](char c) { return head(c) || (c >= '0' && c <= '9'); };
    std::vector<uint64_t> out;
    size_t i = 0;
    while (i < text.size())
    {
        if (!head(text[i]))
        {
            // A digit run is not a name start: "0x1F" gives no name "x1F".
            if (tail(text[i]))
                while (i < text.size() && tail(text[i]))
                    ++i;
            else
                ++i;
            continue;
        }
        const size_t b = i;
        while (i < text.size() && tail(text[i]))
            ++i;
        out.push_back(iface_name_hash(text.substr(b, i - b)));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace detail
{
// True when two sorted lists have a common value.
inline bool sorted_share(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        if (a[i] == b[j])
            return true;
        if (a[i] < b[j])
            ++i;
        else
            ++j;
    }
    return false;
}
} // namespace detail

// The names with a different decl, or with an entry on one side only (rule 5). Sorted.
inline std::vector<uint64_t> iface_changed_names(const IfaceInfo& a, const IfaceInfo& b)
{
    std::vector<uint64_t> d;
    size_t i = 0, j = 0;
    while (i < a.members.size() || j < b.members.size())
    {
        if (j == b.members.size() || (i < a.members.size() && a.members[i].name < b.members[j].name))
            d.push_back(a.members[i++].name);
        else if (i == a.members.size() || b.members[j].name < a.members[i].name)
            d.push_back(b.members[j++].name);
        else
        {
            if (a.members[i].decl != b.members[j].decl)
                d.push_back(a.members[i].name);
            ++i;
            ++j;
        }
    }
    return d;
}

// True when the importer must compile again because the unit "u_name" changes from "live_u"
// (the interface of the live code) to "new_u" (the interface of the new build). "importer" is
// the use data of the importer. "other_imports" has the interfaces of the other imports of the
// importer: the direct ones and the indirect ones, without the unit itself. A null entry is an
// import with no data. The rules, in order:
//  1. The same hash (not zero): not stale. When both sides have fine data, the shape must be
//     the same too: a part that is only in the shape (a stub, a script interface) can change.
//  2. One side has no fine data: stale.
//  3. The shape differs: stale.
//  4. importer.strong has u_name: stale.
//  5. D is the set of the member names with a different decl or with an entry on one side only.
//  6. D and importer.names share a name: stale.
//  7. D and the names of an other import share a name: stale. An import with no data is stale.
//  8. Else: not stale.
// Compare the live unit with the new unit, not with a recorded hash: the rule is then correct
// for a sequence of edits and for a revert to the AOT code.
inline bool importer_is_stale(const IfaceInfo& live_u, const IfaceInfo& new_u, const UseInfo& importer,
    const std::string& u_name, const std::vector<const IfaceInfo*>& other_imports)
{
    if (live_u.hash != 0 && live_u.hash == new_u.hash && (!live_u.known || !new_u.known || live_u.shape == new_u.shape))
        return false;
    if (!live_u.known || !new_u.known || !importer.known)
        return true;
    if (live_u.shape != new_u.shape)
        return true;
    if (std::find(importer.strong.begin(), importer.strong.end(), u_name) != importer.strong.end())
        return true;
    const std::vector<uint64_t> d = iface_changed_names(live_u, new_u);
    if (detail::sorted_share(d, importer.names))
        return true;
    for (const IfaceInfo* other : other_imports)
    {
        if (other == &live_u || other == &new_u)
            continue;
        if (!other || !other->known || detail::sorted_share(d, other->names))
            return true;
    }
    return false;
}

// A text form of IfaceInfo for a cache: "hash shape;name:decl ...;name ...", all numbers in hex.
// An unknown IfaceInfo gives an empty text.
inline std::string encode_iface_info(const IfaceInfo& info)
{
    if (!info.known)
        return {};
    auto hex = [](uint64_t v)
    {
        char b[20];
        std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
        return std::string(b);
    };
    std::string o = hex(info.hash) + " " + hex(info.shape) + ";";
    for (size_t i = 0; i < info.members.size(); ++i)
        o += (i ? " " : "") + hex(info.members[i].name) + ":" + hex(info.members[i].decl);
    o += ";";
    for (size_t i = 0; i < info.names.size(); ++i)
        o += (i ? " " : "") + hex(info.names[i]);
    return o;
}

namespace detail
{
// Reads one hex number at "p" and moves "p" past it. False when no digit is there.
inline bool read_hex(const char*& p, const char* end, uint64_t& v)
{
    v = 0;
    const char* b = p;
    for (; p < end; ++p)
    {
        const char c = *p;
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0 || p - b >= 16)
            break;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return p > b && p - b <= 16;
}
} // namespace detail

// The inverse of encode_iface_info. An empty or bad text gives an unknown IfaceInfo and false
// for a bad text. An unknown IfaceInfo is "no fine data" (rule 2). A text is bad when the
// members are not in strictly ascending order of the name, or the names are not in strictly
// ascending order: the rules need sorted lists with one entry for each name.
inline bool decode_iface_info(std::string_view text, IfaceInfo& info)
{
    info = IfaceInfo{};
    if (text.empty())
        return true;
    IfaceInfo r;
    const char* p = text.data();
    const char* end = p + text.size();
    auto expect = [&](char c)
    {
        if (p < end && *p == c)
        {
            ++p;
            return true;
        }
        return false;
    };
    if (!detail::read_hex(p, end, r.hash) || !expect(' ') || !detail::read_hex(p, end, r.shape) || !expect(';'))
        return false;
    while (p < end && *p != ';')
    {
        IfaceMember m;
        if (!r.members.empty() && !expect(' '))
            return false;
        if (!detail::read_hex(p, end, m.name) || !expect(':') || !detail::read_hex(p, end, m.decl))
            return false;
        if (!r.members.empty() && r.members.back().name >= m.name)
            return false;
        r.members.push_back(m);
    }
    if (!expect(';'))
        return false;
    while (p < end)
    {
        uint64_t v = 0;
        if (!r.names.empty() && !expect(' '))
            return false;
        if (!detail::read_hex(p, end, v))
            return false;
        if (!r.names.empty() && r.names.back() >= v)
            return false;
        r.names.push_back(v);
    }
    r.known = true;
    info = std::move(r);
    return true;
}

// A text form of UseInfo::names for a cache, in hex. The strong list is a separate list of texts.
inline std::string encode_use_names(const std::vector<uint64_t>& names)
{
    IfaceInfo tmp;
    tmp.known = true;
    tmp.names = names;
    return encode_iface_info(tmp);
}

// The inverse of encode_use_names. False for a bad text.
inline bool decode_use_names(std::string_view text, std::vector<uint64_t>& names)
{
    IfaceInfo tmp;
    if (!decode_iface_info(text, tmp) || !tmp.known)
        return false;
    names = std::move(tmp.names);
    return true;
}

} // namespace hot
