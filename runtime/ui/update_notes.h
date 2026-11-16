// The update dialog's changelog (runtime/ui/launcher.mm), from the releases JSON the update check already fetched.
// Releases: the notes of every release between this build and the one offered, newest first, without the warning
// banner on top or the "Built from" line and sha256 table at the bottom (release.yml adds those). Nightlies: the
// rounds of the nightly page's "Changes" list newer than this build (nightly.yml writes one per round, each starting
// with a "<!-- nightly-changes: <stamp> -->" line). Test: tools/test_update_tags.cpp (no network).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <string>
#include <vector>

#include "update_tags.h"

struct ReleaseInfo {
    std::string tag, body;
};

// A JSON string starting at s[i] == '"' -> its text (\uXXXX as UTF-8); i ends after the closing quote. false: broken.
static inline bool json_string(const std::string &s, size_t &i, std::string *out) {
    if (i >= s.size() || s[i] != '"') return false;
    for (i++; i < s.size(); i++) {
        char c = s[i];
        if (c == '"') { i++; return true; }
        if (c != '\\') { if (out) *out += c; continue; }
        if (++i >= s.size()) return false;
        char e = s[i];
        if (e == 'u') {
            if (i + 4 >= s.size()) return false;
            unsigned v = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), NULL, 16);
            i += 4;
            if (v >= 0xd800 && v < 0xdc00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {  // (a pair)
                unsigned lo = (unsigned)strtoul(s.substr(i + 3, 4).c_str(), NULL, 16);
                if (lo >= 0xdc00 && lo < 0xe000) v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00), i += 6;
            }
            if (!out) continue;
            if (v < 0x80) *out += (char)v;
            else if (v < 0x800) *out += (char)(0xc0 | v >> 6), *out += (char)(0x80 | (v & 63));
            else if (v < 0x10000) *out += (char)(0xe0 | v >> 12), *out += (char)(0x80 | (v >> 6 & 63)), *out += (char)(0x80 | (v & 63));
            else *out += (char)(0xf0 | v >> 18), *out += (char)(0x80 | (v >> 12 & 63)), *out += (char)(0x80 | (v >> 6 & 63)), *out += (char)(0x80 | (v & 63));
        } else if (out) *out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
    }
    return false;
}
// Skips one JSON value at s[i] (whitespace first); false: broken
static inline bool json_skip(const std::string &s, size_t &i) {
    while (i < s.size() && strchr(" \t\r\n", s[i])) i++;
    if (i >= s.size()) return false;
    if (s[i] == '"') return json_string(s, i, NULL);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); i++) {
            if (s[i] == '"') { if (!json_string(s, i, NULL)) return false; i--; continue; }
            if (s[i] == '{' || s[i] == '[') depth++;
            else if ((s[i] == '}' || s[i] == ']') && --depth == 0) { i++; return true; }
        }
        return false;
    }
    while (i < s.size() && !strchr(",}] \t\r\n", s[i])) i++;  // (number, true, false, null)
    return true;
}
// The releases list (GitHub's /releases: an array of objects): each one's tag_name and body
static inline std::vector<ReleaseInfo> json_releases(const std::string &s) {
    std::vector<ReleaseInfo> out;
    size_t i = s.find('[');
    if (i == std::string::npos) return out;
    for (i++;;) {
        while (i < s.size() && strchr(" \t\r\n,", s[i])) i++;
        if (i >= s.size() || s[i] != '{') break;
        ReleaseInfo r;
        for (i++;;) {  // the object's keys (nested objects, e.g. author and assets, skipped whole)
            while (i < s.size() && strchr(" \t\r\n,", s[i])) i++;
            if (i >= s.size()) return out;
            if (s[i] == '}') { i++; break; }
            std::string key;
            if (!json_string(s, i, &key)) return out;
            while (i < s.size() && strchr(" \t\r\n:", s[i])) i++;
            if ((key == "tag_name" || key == "body") && i < s.size() && s[i] == '"') {
                if (!json_string(s, i, key == "tag_name" ? &r.tag : &r.body)) return out;
            } else if (!json_skip(s, i)) return out;
        }
        if (!r.tag.empty()) out.push_back(r);
    }
    return out;
}

// A release's notes as the dialog shows them: no warning banner (a leading "> [!WARNING]" quote), nothing from the
// "Built from" line / "---" before it or the sha256 table on, no HTML comments, CRs gone, no blank lines at the ends.
static inline std::string clean_release_notes(std::string b) {
    std::string out;
    std::vector<std::string> lines;
    for (size_t at = 0; at <= b.size();) {
        size_t nl = b.find('\n', at);
        if (nl == std::string::npos) nl = b.size();
        std::string l = b.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        at = nl + 1;
    }
    size_t i = 0;
    while (i < lines.size() && lines[i].empty()) i++;
    if (i < lines.size() && !lines[i].compare(0, 4, "> [!")) {  // (the banner: the quote's lines)
        while (i < lines.size() && !lines[i].compare(0, 1, ">")) i++;
    }
    size_t end = lines.size();
    for (size_t k = i; k < lines.size(); k++) {
        if (!lines[k].compare(0, 11, "Built from ") || !lines[k].compare(0, 9, "| File | ")) {
            end = k;
            while (end > i && (lines[end - 1].empty() || lines[end - 1] == "---")) end--;
            break;
        }
    }
    bool comment = false;
    for (size_t k = i; k < end; k++) {
        std::string l = lines[k];
        bool had = !l.empty();
        for (size_t c; ;) {  // (HTML comments, also across lines)
            if (comment) {
                size_t e = l.find("-->");
                if (e == std::string::npos) { l.clear(); break; }
                l.erase(0, e + 3), comment = false;
            }
            if ((c = l.find("<!--")) == std::string::npos) break;
            size_t e = l.find("-->", c);
            if (e == std::string::npos) { l.erase(c), comment = true; break; }
            l.erase(c, e + 3 - c);
        }
        if (l.empty() && had) continue;  // (a comment's line)
        if (l.empty() && (out.empty() || (out.size() >= 2 && !out.compare(out.size() - 2, 2, "\n\n")))) continue;
        out += l + "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// The changelog from this build (rank own) up to and including the offered release, newest first: each release's
// clean notes under its name ("## Beta 7"). Releases without notes still get their heading.
static inline std::string release_changelog(const std::vector<ReleaseInfo> &rel, long long own, const std::string &offered) {
    long long top = tag_rank(offered);
    std::vector<const ReleaseInfo *> pick;
    for (const ReleaseInfo &r : rel) {
        long long k = tag_rank(r.tag);
        if (k > own && k <= top) pick.push_back(&r);
    }
    for (size_t a = 1; a < pick.size(); a++)  // (newest first)
        for (size_t b = a; b > 0 && tag_rank(pick[b]->tag) > tag_rank(pick[b - 1]->tag); b--) std::swap(pick[b], pick[b - 1]);
    std::string out;
    for (const ReleaseInfo *r : pick) {
        std::string notes = clean_release_notes(r->body);
        // (the notes' own first heading names the release already: "# LAS Recomp Beta 6 Changelog")
        if (notes.compare(0, 2, "# ")) out += "# " + tag_label(r->tag, false) + "\n\n";
        if (!notes.empty()) out += notes + "\n\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// The nightly page's rounds newer than this build's stamp (each "<!-- nightly-changes: <stamp> -->" to the next)
static inline std::string nightly_changelog(const std::vector<ReleaseInfo> &rel, long long own_stamp) {
    std::string body;
    for (const ReleaseInfo &r : rel)
        if (r.tag == "nightly") body = r.body;
    const std::string key = "<!-- nightly-changes: ";
    std::string out;
    for (size_t at = 0; (at = body.find(key, at)) != std::string::npos;) {
        long long stamp = atoll(body.c_str() + at + key.size());
        size_t start = body.find('\n', at);
        size_t next = body.find(key, at + key.size());
        size_t stop = body.find("\nTo report a problem", at);
        size_t end = std::min(next, stop == std::string::npos ? body.size() : stop);
        if (end == std::string::npos) end = body.size();
        at = at + key.size();
        if (stamp <= own_stamp || start == std::string::npos || start >= end) continue;
        out += clean_release_notes(body.substr(start + 1, end - start - 1)) + "\n\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// Markdown, simply: inline marks taken out for the dialog's plain text - **bold**, `code`, [text](url) -> text,
// a bare <url> -> url, \* escapes. (Headings and list items are the caller's: by their line start.)
static inline std::string md_inline(const std::string &l) {
    std::string o;
    for (size_t i = 0; i < l.size(); i++) {
        char c = l[i];
        if (c == '\\' && i + 1 < l.size() && strchr("\\`*_[]()#+-.!<>", l[i + 1])) { o += l[++i]; continue; }
        if (c == '*' && i + 1 < l.size() && l[i + 1] == '*') { i++; continue; }
        if (c == '`') continue;
        if (c == '[') {
            size_t close = l.find("](", i);
            size_t end = close == std::string::npos ? close : l.find(')', close);
            if (end != std::string::npos) { o += md_inline(l.substr(i + 1, close - i - 1)); i = end; continue; }
        }
        if (c == '<' && !l.compare(i + 1, 4, "http")) {
            size_t end = l.find('>', i);
            if (end != std::string::npos) { o += l.substr(i + 1, end - i - 1); i = end; continue; }
        }
        o += c;
    }
    return o;
}
