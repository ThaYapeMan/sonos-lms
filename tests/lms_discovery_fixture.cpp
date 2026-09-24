#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "production_discovery.inc"

int main(int argc, char** argv)
{
    assert(argc == 2);
    const char* path = argv[1];
    assert(readLmsServerFromConfig(path).empty());
    puts("PASS: missing LMS config is optional");
    { std::ofstream file(path); file << "# LMS_SERVER=ignored\n\nOTHER=value\n"; }
    assert(readLmsServerFromConfig(path).empty());
    puts("PASS: missing key, comments and blank config lines are ignored");
    { std::ofstream file(path); file << " \t# comment\n\nOTHER=keep\nLMS_SERVER= \t test.invalid \r\nLMS_SERVER=second.invalid\n"; }
    assert(readLmsServerFromConfig(path) == "test.invalid");
    puts("PASS: first LMS_SERVER value wins and surrounding whitespace is trimmed");
    { std::ofstream file(path); file << "LMS_SERVER= \t\nLMS_SERVER=second.invalid\n"; }
    assert(readLmsServerFromConfig(path).empty());
    puts("PASS: empty first LMS_SERVER value allows discovery");

    std::string name;
    const char named[] = "ENAME\x04" "TestJSON\x04" "9000";
    assert(parseDiscoveryResponse(named, sizeof(named) - 1, name) && name == "Test");
    puts("PASS: discovery NAME tag is extracted from a valid TLV reply");
    const char unnamed[] = "EJSON\x04" "9000";
    assert(parseDiscoveryResponse(unnamed, sizeof(unnamed) - 1, name) && name.empty());
    assert(parseDiscoveryResponse("E", 1, name) && name.empty());
    puts("PASS: valid discovery replies need no NAME tag");
    assert(!parseDiscoveryResponse(nullptr, 0, name));
    assert(!parseDiscoveryResponse("", 0, name));
    assert(!parseDiscoveryResponse("e", 1, name));
    assert(!parseDiscoveryResponse("ENAME", 5, name));
    assert(!parseDiscoveryResponse("ENAME\x04" "abc", 9, name));
    std::string malformed(named, sizeof(named) - 1);
    malformed += 'x';
    name = "stale";
    assert(!parseDiscoveryResponse(malformed.data(), malformed.size(), name) && name.empty());
    puts("PASS: empty, wrong-opcode and truncated discovery replies are rejected");
    std::string longName = "ENAME";
    longName += static_cast<char>(255);
    longName += std::string(255, 'a');
    assert(parseDiscoveryResponse(longName.data(), longName.size(), name));
    assert(name == std::string(255, 'a'));
    puts("PASS: discovery TLV lengths are unsigned bytes");
}
