#include "xml.h"
#include <cctype>
#include <stdexcept>
namespace upnp {
std::string xmlEscape(const std::string& text) {
    std::string out;
    for (char c : text) switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
    }
    return out;
}
namespace {
bool xmlCharacter(unsigned c) {
    return c == 9 || c == 10 || c == 13 || (c >= 32 && c <= 0xd7ff)
        || (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
void utf8(unsigned c, std::string& out) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
    else if (c < 0x10000) {
        out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63));
    } else {
        out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63));
        out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63));
    }
}
std::string local(const std::string& name) {
    const auto colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}
}
bool xmlUnescape(const std::string& text, std::string& out) {
    out.clear();
    for (size_t p = 0; p < text.size(); ++p) {
        if (text[p] != '&') {
            if (!xmlCharacter(static_cast<unsigned char>(text[p]))) return false;
            out += text[p]; continue;
        }
        auto end = text.find(';', p + 1);
        if (end == std::string::npos) return false;
        const auto entity = text.substr(p + 1, end - p - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (!entity.empty() && entity[0] == '#') {
            size_t i = 1; unsigned base = 10, code = 0;
            if (i < entity.size() && entity[i] == 'x') { base = 16; ++i; }
            if (i == entity.size()) return false;
            for (; i < entity.size(); ++i) {
                char c = entity[i];
                unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
                if (digit >= base || code > 0x10ffff / base) return false;
                code = code * base + digit;
            }
            if (!xmlCharacter(code)) return false;
            utf8(code, out);
        } else return false;
        p = end;
    }
    return true;
}
const XmlNode* XmlNode::child(const std::string& n) const {
    for (const auto& c : children) if (local(c.name) == n) return &c;
    return nullptr;
}
std::string XmlNode::value(const std::string& n) const { const auto c = child(n); return c ? c->text : ""; }
std::string XmlNode::attribute(const std::string& n) const { auto i = attributes.find(n); return i == attributes.end() ? "" : i->second; }
namespace {
class Reader {
    const std::string& s;
    size_t p = 0;
    void require(bool ok) { if (!ok) throw std::runtime_error("XML"); }
    bool at(const std::string& v) const { return s.compare(p, v.size(), v) == 0; }
    void space() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    std::string name() {
        size_t start = p;
        require(p < s.size() && (std::isalpha(static_cast<unsigned char>(s[p])) || s[p] == '_'));
        while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_' || s[p] == ':' || s[p] == '-' || s[p] == '.')) ++p;
        return s.substr(start, p - start);
    }
    std::string decode(const std::string& in) { std::string out; require(xmlUnescape(in, out)); return out; }
    bool misc() {
        const bool comment = at("<!--"), pi = at("<?");
        if (!comment && !pi) return false;
        auto end = s.find(comment ? "-->" : "?>", p + (comment ? 4 : 2));
        require(end != std::string::npos); p = end + (comment ? 3 : 2); return true;
    }
    XmlNode element(unsigned depth) {
        require(depth <= 64 && at("<")); ++p;
        XmlNode n; n.name = name();
        for (;;) {
            size_t before = p; space();
            if (at("/>")) { p += 2; return n; }
            if (at(">")) { ++p; break; }
            require(p > before);
            auto key = name(); space(); require(at("=")); ++p; space();
            require(p < s.size() && (s[p] == '\'' || s[p] == '"'));
            char quote = s[p++]; auto end = s.find(quote, p); require(end != std::string::npos);
            auto val = s.substr(p, end - p); require(val.find('<') == std::string::npos);
            require(n.attributes.emplace(key, decode(val)).second); p = end + 1;
        }
        for (;;) {
            require(p < s.size());
            if (at("</")) { p += 2; require(name() == n.name); space(); require(at(">")); ++p; return n; }
            if (misc()) continue;
            if (at("<![CDATA[")) {
                p += 9; auto end = s.find("]]>", p); require(end != std::string::npos);
                n.text += s.substr(p, end - p); p = end + 3;
            } else if (at("<")) n.children.push_back(element(depth + 1));
            else {
                auto end = s.find('<', p); require(end != std::string::npos);
                n.text += decode(s.substr(p, end - p)); p = end;
            }
        }
    }
public:
    explicit Reader(const std::string& in) : s(in) {}
    XmlNode read() {
        space(); while (misc()) space();
        auto root = element(0); space(); while (misc()) space();
        require(p == s.size()); return root;
    }
};
}
bool parseXml(const std::string& text, XmlNode& root) {
    root = {};
    if (text.size() > 2 * 1024 * 1024) return false;
    try { root = Reader(text).read(); return true; } catch (const std::runtime_error&) { return false; }
}
}
