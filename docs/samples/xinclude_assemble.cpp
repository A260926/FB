#include "pugixml.hpp"
#include "extras/xinclude.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// tag::code[]
// An offline resource table: every resource of the package lives in memory.
// The XInclude assembler never opens a file or a socket itself; it only calls
// the resolver with the absolute, dot-segment-normalized URI it needs.
class MemoryPackage
{
public:
    // Stores a resource under its absolute URI.
    void store(const std::string& uri, const std::string& bytes)
    {
        resources_[uri].assign(bytes.begin(), bytes.end());
    }

    // The resolver handed to pugi::xinclude::assemble. A null data pointer in
    // the returned Resource means "resource does not exist".
    pugi::xinclude::ResourceResolver resolver()
    {
        return [this](const std::string& uri) -> pugi::xinclude::Resource
        {
            std::map<std::string, std::vector<std::uint8_t> >::iterator it = resources_.find(uri);
            if (it == resources_.end())
                return pugi::xinclude::Resource::not_found();

            return pugi::xinclude::Resource(it->second.data(), it->second.size());
        };
    }

private:
    std::map<std::string, std::vector<std::uint8_t> > resources_;
};
// end::code[]

int main()
{
    static const char* XI = "http://www.w3.org/2001/XInclude";
    const std::string main_uri = "https://example.test/package/main.xml";

    MemoryPackage package;

    // Main document: a relative XML include, a text include and a missing
    // resource covered by an xi:fallback.
    const std::string main_document =
        std::string("<book xmlns:xi='") + XI + "'>"
        "  <xi:include href='chapters/intro.xml'/>"
        "  <xi:include href='notes.txt' parse='text'/>"
        "  <xi:include href='absent.xml'>"
        "    <xi:fallback><para>fallback content</para></xi:fallback>"
        "  </xi:include>"
        "</book>";
    package.store(main_uri, main_document);

    // The referenced booklet, which itself contains another include.
    package.store("https://example.test/package/chapters/intro.xml",
        std::string("<chapter xmlns:xi='") + XI + "'>"
        "  <title>Introduction</title>"
        "  <xi:include href='section.xml'/>"
        "</chapter>");
    package.store("https://example.test/package/chapters/section.xml",
        "<section>nested section body</section>");

    // A plain-text resource (UTF-8 here; UTF-16LE/BE are also accepted).
    // CRLF and lone CR line endings are normalized to LF.
    package.store("https://example.test/package/notes.txt",
        "first line\r\nsecond line\r");

    // Parse the main document normally; pugixml keeps xi:include as ordinary
    // elements until assembly is explicitly requested.
    pugi::xml_document input;
    pugi::xml_parse_result parsed = input.load_string(main_document.c_str());
    if (!parsed)
    {
        std::cerr << "main document parse failed: " << parsed.description() << "\n";
        return 1;
    }

    // Assemble a new, self-contained document. The input document is left
    // untouched and no node handle is moved out of it.
    pugi::xinclude::Result result =
        pugi::xinclude::assemble(input, main_uri, package.resolver());

    if (!result)
    {
        std::cerr << "assembly failed: " << result.error.message << "\n"
                  << "  resource: " << result.error.resource_uri << "\n"
                  << "  include chain (main document first):\n";
        for (std::size_t i = 0; i < result.error.include_chain.size(); ++i)
            std::cerr << "    " << i << ": " << result.error.include_chain[i] << "\n";
        return 1;
    }

    result.document.save(std::cout, "  ");
    return 0;
}
