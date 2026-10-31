// The configurable name of the standard namespace (the module attribute "[std("eastl")];").
//
// The translator writes the placeholder kStdPlaceholder (the byte 0x01 and "std::") for each
// standard library name that it emits. User text cannot hold this byte. A script name "std.x" is
// the placeholder and "x" in the type text. A stub string of the user writes "$std::" for it
// (see std_from_user). One call of expand_std() replaces the
// placeholder in the final text. The default namespace is "std".
//
// The text from libclang uses the real namespace. unexpand_std() maps it back to the placeholder,
// so a stub and an override match the native text for each namespace name.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hotc
{

inline constexpr std::string_view kStdPlaceholder = "\x01std::";

// Converts the "$std::" of a stub string (the user form) to the internal placeholder. Call it
// where the translator reads the string, and nowhere else.
inline std::string std_from_user(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();)
    {
        if (text.compare(i, 6, "$std::") == 0)
        {
            out += kStdPlaceholder;
            i += 6;
        }
        else
            out += text[i++];
    }
    return out;
}

// The namespace of the current call (default "std"). The translator sets it when it reads the
// module attribute list of the preludes. The libclang readers use it to map the native text.
inline std::string& current_std_ns()
{
    static thread_local std::string ns = "std";
    return ns;
}

// True for a C++ namespace name: identifiers that "::" joins, for example "eastl" or "a::b".
inline bool valid_std_ns(std::string_view ns)
{
    if (ns.empty())
        return false;
    size_t i = 0;
    while (true)
    {
        if (i >= ns.size() || !(std::isalpha(static_cast<unsigned char>(ns[i])) || ns[i] == '_'))
            return false;
        while (i < ns.size() && (std::isalnum(static_cast<unsigned char>(ns[i])) || ns[i] == '_'))
            ++i;
        if (i == ns.size())
            return true;
        if (ns.compare(i, 2, "::") != 0)
            return false;
        i += 2;
    }
}

// Replaces each "\x01std::" in the text with "<ns>::".
inline std::string expand_std(std::string_view text, std::string_view ns)
{
    std::string out;
    out.reserve(text.size());
    const std::string repl = std::string(ns) + "::";
    size_t i = 0;
    while (i < text.size())
    {
        const size_t p = text.find(kStdPlaceholder, i);
        if (p == std::string_view::npos)
        {
            out.append(text.substr(i));
            break;
        }
        out.append(text.substr(i, p - i));
        out += repl;
        i = p + kStdPlaceholder.size();
    }
    return out;
}

// Like expand_std(), and it moves the generated offsets of the segments. A segment type has
// the fields "gen_begin" and "gen_len".
template <class Segment>
std::string expand_std(std::string_view text, std::string_view ns, std::vector<Segment>& segments)
{
    // The offset of each placeholder in the old text.
    std::vector<size_t> at;
    for (size_t p = text.find(kStdPlaceholder); p != std::string_view::npos;
         p = text.find(kStdPlaceholder, p + kStdPlaceholder.size()))
        at.push_back(p);
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(ns.size() + 2) - static_cast<std::ptrdiff_t>(kStdPlaceholder.size());
    // The number of placeholders that start before the offset.
    auto before = [&](size_t off)
    {
        size_t lo = 0, hi = at.size();
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (at[mid] < off)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    };
    for (Segment& s : segments)
    {
        const size_t b = before(s.gen_begin), e = before(static_cast<size_t>(s.gen_begin) + s.gen_len);
        s.gen_begin = static_cast<decltype(s.gen_begin)>(static_cast<std::ptrdiff_t>(s.gen_begin) + delta * static_cast<std::ptrdiff_t>(b));
        s.gen_len = static_cast<decltype(s.gen_len)>(
            static_cast<std::ptrdiff_t>(s.gen_len) + delta * static_cast<std::ptrdiff_t>(e - b));
    }
    return expand_std(text, ns);
}

// Replaces each "<ns>::" that starts a qualified name with "\x01std::". A name that "::" or the
// placeholder byte or a word character comes before is not the start: "my::std::x" stays.
inline std::string unexpand_std(std::string_view text, std::string_view ns)
{
    const std::string from = std::string(ns) + "::";
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size())
    {
        const size_t p = text.find(from, i);
        if (p == std::string_view::npos)
        {
            out.append(text.substr(i));
            break;
        }
        out.append(text.substr(i, p - i));
        const char prev = p == 0 ? ' ' : text[p - 1];
        const bool word = std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == ':' || prev == '\x01';
        out += word ? from : std::string(kStdPlaceholder);
        i = p + from.size();
    }
    return out;
}

// The mapping of the native text with the namespace of the current call.
inline std::string unexpand_std(std::string_view text)
{
    return unexpand_std(text, current_std_ns());
}

} // namespace hotc
