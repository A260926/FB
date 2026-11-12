// Target tests for the XInclude assembly helper.
// All resource materials are built in memory; no files are downloaded.

#include "cases.hpp"
#include "extras/xinclude.hpp"

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	using pugi::xinclude::Resource;
	using pugi::xinclude::ResourceResolver;
	using pugi::xinclude::Result;

	// In-memory resource table: the resolver never touches the file system.
	struct ResourceTable
	{
		std::map<std::string, std::vector<std::uint8_t> > entries;
		std::vector<std::string> requested;

		void put(const std::string& uri, const std::string& body)
		{
			entries[uri].assign(body.begin(), body.end());
		}

		void put_bytes(const std::string& uri, const std::vector<std::uint8_t>& body)
		{
			entries[uri] = body;
		}

		ResourceResolver callback()
		{
			return [this](const std::string& uri) -> Resource
			{
				requested.push_back(uri);
				std::map<std::string, std::vector<std::uint8_t> >::iterator it = entries.find(uri);
				if (it == entries.end()) return Resource::not_found();
				return Resource(it->second.data(), it->second.size());
			};
		}
	};

	std::string dump(const pugi::xml_node& node)
	{
		std::ostringstream out;
		node.print(out, "", pugi::format_raw | pugi::format_no_declaration);
		return out.str();
	}

	bool load(pugi::xml_document& doc, const ResourceTable& table, const std::string& uri)
	{
		const std::vector<std::uint8_t>& bytes = table.entries.at(uri);
		return doc.load_buffer(bytes.data(), bytes.size());
	}

	std::vector<std::uint8_t> utf16le(const std::string& utf8, bool bom)
	{
		// Test helper input only needs ASCII plus U+00E9.
		std::vector<std::uint8_t> out;
		if (bom) { out.push_back(0xFF); out.push_back(0xFE); }
		for (std::size_t i = 0; i < utf8.size(); ++i)
		{
			unsigned char c = static_cast<unsigned char>(utf8[i]);
			if (c == 0xC3 && i + 1 < utf8.size() && static_cast<unsigned char>(utf8[i + 1]) == 0xA9)
			{
				out.push_back(0xE9); out.push_back(0x00); // U+00E9 little endian
				++i;
			}
			else
			{
				out.push_back(static_cast<std::uint8_t>(c));
				out.push_back(0x00);
			}
		}
		return out;
	}

	std::vector<std::uint8_t> utf16be_ascii(const std::string& ascii)
	{
		std::vector<std::uint8_t> out;
		for (char c : ascii) { out.push_back(0x00); out.push_back(static_cast<std::uint8_t>(c)); }
		return out;
	}

	int element_children(pugi::xml_node node)
	{
		int count = 0;
		for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling())
			if (c.type() == pugi::node_element) ++count;
		return count;
	}
}

// Nested relative hrefs, inherited xml:base, and the same booklet included
// from two sibling positions.
CASE(recursive_resources)
{
	static const char* NS = "http://www.w3.org/2001/XInclude";
	const std::string main_uri = "https://example.test/pkg/main.xml";

	ResourceTable table;
	table.put(main_uri,
		"<book xmlns:xi='" + std::string(NS) + "'>"
		"<xi:include href='vol/part1.xml'/>"
		"<xi:include href='vol/part1.xml'/>"
		"</book>");

	table.put("https://example.test/pkg/vol/part1.xml",
		"<chapter xmlns:xi='" + std::string(NS) + "' xml:base='sec/'>"
		"<xi:include href='intro.xml'/>"
		"<xi:include href='nested/deep.xml'/>"
		"</chapter>");

	table.put("https://example.test/pkg/vol/sec/intro.xml", "<intro>hi</intro>");

	table.put("https://example.test/pkg/vol/sec/nested/deep.xml",
		"<section xmlns:xi='" + std::string(NS) + "'><xi:include href='../leaf.xml'/></section>");

	table.put("https://example.test/pkg/vol/sec/leaf.xml", "<leaf>deep-leaf</leaf>");

	pugi::xml_document input;
	CHECK(load(input, table, main_uri));

	Result result;
	{
		// The returned document must survive the input document going away.
		pugi::xml_document scoped_input;
		CHECK(load(scoped_input, table, main_uri));
		result = pugi::xinclude::assemble(scoped_input, main_uri, table.callback());
	}

	CHECK(result);
	pugi::xml_node book = result.document.child("book");
	CHECK(book);

	// Sibling-position duplicate reference of the same absolute URI is legal.
	CHECK(element_children(book) == 2);
	pugi::xml_node ch1 = book.first_child();
	pugi::xml_node ch2 = ch1.next_sibling();
	CHECK(std::string(ch1.name()) == "chapter");
	CHECK(std::string(ch2.name()) == "chapter");

	for (pugi::xml_node chapter = book.first_child(); chapter; chapter = chapter.next_sibling())
	{
		CHECK(std::string(chapter.child("intro").child_value()) == "hi");
		pugi::xml_node leaf = chapter.child("section").child("leaf");
		CHECK(leaf);
		CHECK(std::string(leaf.child_value()) == "deep-leaf");
	}

	// Relative addresses were resolved against inherited xml:base, not the
	// main document URI, and dot segments ("../") were removed.
	bool saw_leaf = false, saw_deep = false;
	for (std::size_t i = 0; i < table.requested.size(); ++i)
	{
		if (table.requested[i] == "https://example.test/pkg/vol/sec/leaf.xml") saw_leaf = true;
		if (table.requested[i] == "https://example.test/pkg/vol/sec/nested/deep.xml") saw_deep = true;
		CHECK(table.requested[i].find("/./") == std::string::npos);
		CHECK(table.requested[i].find("/../") == std::string::npos);
	}
	CHECK(saw_leaf);
	CHECK(saw_deep);

	// The input document is unchanged and still owns its include directives.
	pugi::xml_node in_book = input.child("book");
	CHECK(element_children(in_book) == 2);
	CHECK(in_book.first_child().attribute("href"));
	CHECK(std::string(in_book.first_child().attribute("href").value()) == "vol/part1.xml");
}

// element() selection plus preservation of namespace, language and base
// context once the selected subtree is relocated.
CASE(selected_context)
{
	static const char* NS = "http://www.w3.org/2001/XInclude";
	const std::string main_uri = "https://x.example/a/main.xml";

	ResourceTable table;
	table.put(main_uri,
		"<doc xmlns:xi='" + std::string(NS) + "'>"
		"<xi:include href='r/part.xml' xpointer='element(/1/2)'/>"
		"</doc>");

	table.put("https://x.example/a/r/part.xml",
		"<root xmlns:n='urn:ns:n' xmlns:m='urn:ns:m' xml:lang='en' xml:base='sub/dir/'>"
		"<n:first><a/></n:first>"
		"<m:second>"
		"<m:inner link='rel/page.xml'>text</m:inner>"
		"</m:second>"
		"</root>");

	pugi::xml_document input;
	CHECK(load(input, table, main_uri));

	Result result = pugi::xinclude::assemble(input, main_uri, table.callback());
	CHECK(result);

	pugi::xml_node second = result.document.child("doc").first_child();
	CHECK(second);
	CHECK(std::string(second.name()) == "m:second");

	std::string serialized = dump(second);

	// Ancestor namespace bindings must travel with the detached subtree.
	CHECK(serialized.find("xmlns:m=\"urn:ns:m\"") != std::string::npos);
	CHECK(serialized.find("xmlns:n=\"urn:ns:n\"") != std::string::npos);

	// Inherited xml:lang is materialized explicitly.
	CHECK(std::string(second.attribute("xml:lang").value()) == "en");

	// The effective absolute xml:base is preserved so the relative link keeps
	// its original meaning after relocation.
	CHECK(std::string(second.attribute("xml:base").value()) == "https://x.example/a/r/sub/dir/");
	pugi::xml_node inner = second.first_child();
	CHECK(std::string(inner.name()) == "m:inner");
	CHECK(std::string(inner.attribute("link").value()) == "rel/page.xml");
	CHECK(std::string(inner.child_value()) == "text");

	// element(/1) selects the document element itself and keeps its own
	// namespace declarations.
	ResourceTable table2;
	table2.put("https://x.example/a/r/part.xml",
		"<root xmlns:n='urn:ns:n' xmlns:m='urn:ns:m' xml:lang='en' xml:base='sub/dir/'>"
		"<n:first><a/></n:first>"
		"<m:second><m:inner link='rel/page.xml'>text</m:inner></m:second>"
		"</root>");
	table2.put(main_uri,
		"<doc xmlns:xi='" + std::string(NS) + "'>"
		"<xi:include href='r/part.xml' xpointer='element(/1)'/>"
		"</doc>");
	pugi::xml_document input2;
	CHECK(load(input2, table2, main_uri));
	Result r2 = pugi::xinclude::assemble(input2, main_uri, table2.callback());
	CHECK(r2);
	pugi::xml_node root_copy = r2.document.child("doc").child("root");
	CHECK(root_copy);
	CHECK(std::string(root_copy.attribute("xmlns:n").value()) == "urn:ns:n");
}

// Text byte decoding, line ending normalization, text-node insertion and
// ordered fallback replacement.
CASE(text_fallback)
{
	static const char* NS = "http://www.w3.org/2001/XInclude";
	const std::string main_uri = "https://t.example/main.xml";

	ResourceTable table;

	table.put("https://t.example/u8.txt", "line1\r\nline2\rend<not-an-element/>");
	table.put_bytes("https://t.example/u16le.txt", utf16le("A\r\nB\rC\xC3\xA9", true));
	table.put_bytes("https://t.example/u16be.txt",
		utf16be_ascii("<?xml version=\"1.0\" encoding=\"UTF-16BE\"?>plain"));

	// BOM says UTF-8 while the declaration says UTF-16LE: conflict, must fail.
	{
		std::vector<std::uint8_t> conflict;
		conflict.push_back(0xEF); conflict.push_back(0xBB); conflict.push_back(0xBF);
		std::string decl = "<?xml version=\"1.0\" encoding=\"UTF-16LE\"?>";
		conflict.insert(conflict.end(), decl.begin(), decl.end());
		table.put_bytes("https://t.example/conflict.txt", conflict);
	}

	// Malformed UTF-8: leading byte without continuation.
	table.put_bytes("https://t.example/bad.txt", std::vector<std::uint8_t>({ 'o', 'k', 0xC3, 0x28 }));

	table.put("https://t.example/ok.txt", "fb-text");

	table.put(main_uri,
		"<root xmlns:xi='" + std::string(NS) + "'>"
		"<wrap><xi:include href='u8.txt' parse='text'/></wrap>"
		"<wrap16><xi:include href='u16le.txt' parse='text'/></wrap16>"
		"<wrapbe><xi:include href='u16be.txt' parse='text'/></wrapbe>"
		"<ordered>"
		"<xi:include href='missing.txt' parse='text'>"
		"<xi:fallback><z>one</z><z>two</z></xi:fallback>"
		"</xi:include>"
		"</ordered>"
		"<nested>"
		"<xi:include href='conflict.txt' parse='text'>"
		"<xi:fallback>"
		"<xi:include href='bad.txt' parse='text'>"
		"<xi:fallback><xi:include href='ok.txt' parse='text'/></xi:fallback>"
		"</xi:include>"
		"</xi:fallback>"
		"</xi:include>"
		"</nested>"
		"</root>");

	pugi::xml_document input;
	CHECK(load(input, table, main_uri));
	Result result = pugi::xinclude::assemble(input, main_uri, table.callback());
	CHECK(result);

	pugi::xml_node root = result.document.child("root");

	// UTF-8: CRLF and CR collapse to LF; content stays a single text node and
	// is never interpreted as XML markup.
	pugi::xml_node wrap = root.child("wrap");
	CHECK(wrap.first_child().type() == pugi::node_pcdata);
	CHECK(std::string(wrap.first_child().value()) == "line1\nline2\nend<not-an-element/>");
	CHECK(!wrap.child("not-an-element"));

	// UTF-16LE with BOM: BOM removed, surrogates decoded to UTF-8, EOL fixed.
	pugi::xml_node wrap16 = root.child("wrap16");
	CHECK(std::string(wrap16.first_child().value()) == std::string("A\nB\nC\xC3\xA9"));

	// UTF-16BE detected from the encoding declaration (no BOM); the declaration
	// bytes are ordinary text in parse='text' mode.
	pugi::xml_node wrapbe = root.child("wrapbe");
	std::string be_text = wrapbe.first_child().value();
	CHECK(be_text.compare(0, 5, "<?xml") == 0);
	CHECK(be_text.compare(be_text.size() - 5, 5, "plain") == 0);

	// Fallback children are inserted in their original order.
	pugi::xml_node ordered = root.child("ordered");
	pugi::xml_node z1 = ordered.first_child();
	CHECK(std::string(z1.name()) == "z" && std::string(z1.child_value()) == "one");
	pugi::xml_node z2 = z1.next_sibling();
	CHECK(std::string(z2.name()) == "z" && std::string(z2.child_value()) == "two");
	CHECK(!z2.next_sibling());

	// Both fallbackable decoding failures unwind to the innermost available
	// fallback, whose own include is then processed.
	pugi::xml_node nested = root.child("nested");
	CHECK(std::string(nested.first_child().value()) == "fb-text");
}

// The line between recoverable (fallbackable) failures and structural errors
// that fallback must not swallow.
CASE(structural_errors)
{
	static const char* NS = "http://www.w3.org/2001/XInclude";
	const std::string main_uri = "https://s.example/c/main.xml";
	const std::string xi = "xmlns:xi='" + std::string(NS) + "'";

	// Recoverable failures with a fallback succeed.
	{
		ResourceTable table;
		table.put("https://s.example/c/broken.xml", "<root><unclosed>");
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='missing.xml'><xi:fallback><fb id='missing'/></xi:fallback></xi:include>"
			"<xi:include href='broken.xml'><xi:fallback><fb id='broken'/></xi:fallback></xi:include>"
			"<xi:include href='broken.xml' xpointer='element(/1/9)'><xi:fallback><fb id='empty'/></xi:fallback></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(r);
		pugi::xml_node doc = r.document.child("doc");
		CHECK(std::string(doc.child("fb").attribute("id").value()) == "missing");
		CHECK(std::string(doc.find_child_by_attribute("fb", "id", "broken").attribute("id").value()) == "broken");
		CHECK(doc.find_child_by_attribute("fb", "id", "empty"));
	}

	// The same failures without a fallback produce typed diagnostics.
	{
		ResourceTable table;
		table.put("https://s.example/c/broken.xml", "<root><unclosed>");
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='ghost/missing.xml'/>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_resource_not_found);
		CHECK(r.error.resource_uri == "https://s.example/c/ghost/missing.xml");
		CHECK(r.error.include_chain.size() == 2);
		CHECK(r.error.include_chain[0] == main_uri);
		CHECK(r.error.include_chain[1] == "https://s.example/c/ghost/missing.xml");
		// Failure must not modify the input document.
		CHECK(input.child("doc").first_child().attribute("href"));
	}

	// Structural errors are not swallowed by fallback.
	{
		ResourceTable table;
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='x.xml' parse='binary'><xi:fallback><fb/></xi:fallback></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_structural);
		CHECK(!r.document.child("doc")); // failed result document is empty
	}

	{
		ResourceTable table;
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='x.xml'><xi:fallback/><xi:fallback/></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_structural);
	}

	{
		ResourceTable table;
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='x.xml'><bogus/><xi:fallback/></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_structural);
	}

	{
		ResourceTable table;
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='x.xml' xpointer='xpointer(//a)'><xi:fallback/></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_structural);
	}

	// Query and fragment references are rejected even with a fallback.
	{
		ResourceTable table;
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='x.xml?a=1'><xi:fallback/></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_unsupported_syntax);
	}

	{
		ResourceTable table;
		table.put(main_uri,
			"<doc " + xi + ">"
			"<xi:include href='x.xml#frag'><xi:fallback/></xi:include>"
			"</doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_unsupported_syntax);
	}

	// A duplicate URI in the active reference chain is a cycle; the sibling
	// duplicate in recursive_resources is not. Fallback cannot absorb cycles.
	{
		ResourceTable table;
		table.put("https://s.example/c/a.xml",
			"<a " + xi + "><xi:include href='main.xml'><xi:fallback><fb/></xi:fallback></xi:include></a>");
		table.put(main_uri,
			"<doc " + xi + "><xi:include href='a.xml'/></doc>");
		pugi::xml_document input;
		CHECK(load(input, table, main_uri));
		Result r = pugi::xinclude::assemble(input, main_uri, table.callback());
		CHECK(!r);
		CHECK(r.error.code == pugi::xinclude::xi_error_structural);
		CHECK(r.error.include_chain.size() == 3);
		CHECK(r.error.include_chain[0] == main_uri);
		CHECK(r.error.include_chain[1] == "https://s.example/c/a.xml");
		CHECK(r.error.include_chain[2] == main_uri);
		CHECK(!r.document.child("doc"));
	}
}
