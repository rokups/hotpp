// The stale rule of an importer (hot::importer_is_stale) and the text form of the fine data.
// One test for each of the 8 rules, in the order of the rules.

#include "hot/iface_compat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

uint64_t h(const char* name)
{
    return hot::iface_name_hash(name);
}

std::vector<uint64_t> names(std::vector<const char*> list)
{
    std::vector<uint64_t> out;
    for (const char* n : list)
        out.push_back(h(n));
    std::sort(out.begin(), out.end());
    return out;
}

// An interface with the members "list" (name, decl) and the hash "hash".
hot::IfaceInfo iface(uint64_t hash, std::vector<std::pair<const char*, uint64_t>> list, uint64_t shape = 1)
{
    hot::IfaceInfo r;
    r.known = true;
    r.hash = hash;
    r.shape = shape;
    for (const auto& [n, decl] : list)
        r.members.push_back({h(n), decl});
    std::sort(r.members.begin(), r.members.end(),
        [](const hot::IfaceMember& a, const hot::IfaceMember& b) { return a.name < b.name; });
    for (const auto& m : r.members)
        r.names.push_back(m.name);
    return r;
}

hot::UseInfo use(std::vector<const char*> list, std::vector<std::string> strong = {})
{
    hot::UseInfo u;
    u.known = true;
    u.names = names(std::move(list));
    u.strong = std::move(strong);
    return u;
}

const hot::IfaceInfo kLive = iface(10, {{"Get", 1}, {"ticks", 2}});
const hot::IfaceInfo kNewMethod = iface(11, {{"Get", 1}, {"ticks", 2}, {"Reset", 3}});

} // namespace

TEST(IfaceCompat, Rule1SameHashIsNotStale)
{
    hot::IfaceInfo unknown;
    unknown.hash = 10;
    EXPECT_FALSE(hot::importer_is_stale(kLive, unknown, hot::UseInfo{}, "u", {})) << "the hash wins over no data";
    EXPECT_FALSE(hot::importer_is_stale(kLive, kLive, use({"Get"}, {"u"}), "u", {}));
    hot::IfaceInfo reshaped = kLive;
    reshaped.shape = 2;
    EXPECT_TRUE(hot::importer_is_stale(kLive, reshaped, use({}), "u", {}))
        << "the same hash with fine data needs the same shape";
    hot::IfaceInfo zero_a, zero_b;
    EXPECT_TRUE(hot::importer_is_stale(zero_a, zero_b, use({}), "u", {})) << "a zero hash is not known";
}

TEST(IfaceCompat, Rule2NoFineDataIsStale)
{
    hot::IfaceInfo old_aot;
    old_aot.hash = 10;
    EXPECT_TRUE(hot::importer_is_stale(old_aot, kNewMethod, use({}), "u", {}));
    EXPECT_TRUE(hot::importer_is_stale(kLive, hot::IfaceInfo{}, use({}), "u", {}));
    EXPECT_TRUE(hot::importer_is_stale(kLive, kNewMethod, hot::UseInfo{}, "u", {}));
    EXPECT_FALSE(hot::importer_is_stale(kLive, kNewMethod, use({}), "u", {}));
}

TEST(IfaceCompat, Rule3OtherShapeIsStale)
{
    const hot::IfaceInfo shaped = iface(12, {{"Get", 1}, {"ticks", 2}}, 2);
    EXPECT_TRUE(hot::importer_is_stale(kLive, shaped, use({}), "u", {}));
}

TEST(IfaceCompat, Rule4StrongImportIsStale)
{
    EXPECT_TRUE(hot::importer_is_stale(kLive, kNewMethod, use({}, {"u"}), "u", {}));
    EXPECT_FALSE(hot::importer_is_stale(kLive, kNewMethod, use({}, {"other"}), "u", {}));
}

TEST(IfaceCompat, Rule5ChangedNamesAreTheSymmetricDifferenceAndTheOtherDecls)
{
    const hot::IfaceInfo b = iface(12, {{"Get", 9}, {"Reset", 3}});
    std::vector<uint64_t> want = names({"Get", "Reset", "ticks"});
    EXPECT_EQ(hot::iface_changed_names(kLive, b), want);
    EXPECT_EQ(hot::iface_changed_names(b, kLive), want);
    EXPECT_TRUE(hot::iface_changed_names(kLive, kLive).empty());
}

TEST(IfaceCompat, Rule6UsedChangedNameIsStale)
{
    EXPECT_TRUE(hot::importer_is_stale(kLive, kNewMethod, use({"Reset"}), "u", {})) << "a new member can win";
    const hot::IfaceInfo removed = iface(12, {{"Get", 1}});
    EXPECT_TRUE(hot::importer_is_stale(kLive, removed, use({"ticks"}), "u", {})) << "a removed member";
    const hot::IfaceInfo retyped = iface(13, {{"Get", 7}, {"ticks", 2}});
    EXPECT_TRUE(hot::importer_is_stale(kLive, retyped, use({"Get"}), "u", {})) << "a changed member";
}

TEST(IfaceCompat, Rule7ChangedNameInAnOtherImportIsStale)
{
    hot::IfaceInfo mid;
    mid.known = true;
    mid.hash = 50;
    mid.names = names({"Mid", "Reset"});
    EXPECT_TRUE(hot::importer_is_stale(kLive, kNewMethod, use({"Mid"}), "u", {&mid}));
    mid.names = names({"Mid"});
    EXPECT_FALSE(hot::importer_is_stale(kLive, kNewMethod, use({"Mid"}), "u", {&mid}));
    hot::IfaceInfo unknown;
    EXPECT_TRUE(hot::importer_is_stale(kLive, kNewMethod, use({}), "u", {&unknown})) << "an import with no data";
    EXPECT_TRUE(hot::importer_is_stale(kLive, kNewMethod, use({}), "u", {nullptr}));
    EXPECT_FALSE(hot::importer_is_stale(kLive, kNewMethod, use({}), "u", {&kNewMethod, &kLive}))
        << "the unit itself is not an other import";
}

TEST(IfaceCompat, Rule8ElseNotStale)
{
    EXPECT_FALSE(hot::importer_is_stale(kLive, kNewMethod, use({"Get", "ticks", "Widget"}), "u", {}));
    EXPECT_FALSE(hot::importer_is_stale(kNewMethod, kLive, use({"Get"}), "u", {})) << "a revert keeps the importer";
}

TEST(IfaceCompat, NameSetHasEachIdentifierOnce)
{
    EXPECT_EQ(hot::iface_name_set("a b a // c\n\"d e\" 0x1F f2 _g"), names({"a", "b", "c", "d", "e", "f2", "_g"}));
    EXPECT_TRUE(hot::iface_name_set("1 2 ++ 0x10").empty());
}

TEST(IfaceCompat, TextFormRoundTrips)
{
    hot::IfaceInfo a = kNewMethod;
    a.hash = 0xfedcba9876543210ull;
    a.names.push_back(~0ull);
    hot::IfaceInfo b;
    ASSERT_TRUE(hot::decode_iface_info(hot::encode_iface_info(a), b));
    EXPECT_TRUE(a == b);
    hot::IfaceInfo empty = iface(3, {});
    empty.names.clear();
    ASSERT_TRUE(hot::decode_iface_info(hot::encode_iface_info(empty), b));
    EXPECT_TRUE(empty == b);
    ASSERT_TRUE(hot::decode_iface_info("", b));
    EXPECT_FALSE(b.known) << "an empty text is no fine data";
    EXPECT_FALSE(hot::decode_iface_info("1 2;x", b));
    EXPECT_FALSE(b.known);
    EXPECT_FALSE(hot::decode_iface_info("12345678901234567 2;;", b)) << "a number with 17 digits";
    std::vector<uint64_t> n;
    // A bad text gives unknown data (rule 2).
    for (const char* bad : {"1 2;5:1 3:1;", "1 2;3:1 3:2;", "1 2;3:1;9 4", "1 2;3:1;4 4", "1 2;3:1;4 ",
             "1 2;3:1;4 9 ", "1 2;3:1 ;4", "1 2;3:1", "1 2", "1 2;;4 9 2"})
    {
        EXPECT_FALSE(hot::decode_iface_info(bad, b)) << bad;
        EXPECT_FALSE(b.known) << bad;
    }
    EXPECT_FALSE(hot::decode_use_names("0 0;;5 3", n)) << "unsorted names";
    ASSERT_TRUE(hot::decode_use_names(hot::encode_use_names(names({"x", "y"})), n));
    EXPECT_EQ(n, names({"x", "y"}));
    ASSERT_TRUE(hot::decode_use_names(hot::encode_use_names({}), n));
    EXPECT_TRUE(n.empty());
}
