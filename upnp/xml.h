#pragma once
#include <map>
#include <string>
#include <vector>
namespace upnp {
// Text or double-quoted attributes; apostrophes intentionally match noson's bytes.
std::string xmlEscape(const std::string& text);
bool xmlUnescape(const std::string& text, std::string& out);
struct XmlNode {
    std::string name, text;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;
    const XmlNode* child(const std::string& localName) const;
    std::string value(const std::string& localName) const;
    std::string attribute(const std::string& key) const;
};
// Small bounded XML reader: namespaces by local name, entities, comments, CDATA.
// Rejects DTDs/external entities, malformed input, excessive nesting and size.
bool parseXml(const std::string& text, XmlNode& root);
}
