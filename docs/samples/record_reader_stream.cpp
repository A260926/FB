#include "record_reader.hpp"

#include <iostream>
#include <string>

// Incremental consumption of a large record-oriented XML feed. The reader owns a small staging buffer
// and hands each direct child of the root element to the callback the moment its closing tag is parsed;
// neither the whole document nor its source bytes have to live in memory at once.

namespace
{
	// Per-record processing state passed through the opaque user pointer.
	struct record_sink
	{
		size_t count = 0;
	};

	void on_record(const pugi::xml_document& doc, void* userdata)
	{
		record_sink& sink = *static_cast<record_sink*>(userdata);

		// The callback document contains the root element with exactly one child: the just-closed
		// record. Handles are valid only for the duration of this call.
		pugi::xml_node record = doc.document_element().first_child();

		// Read an attribute and the record text. Both are available through the ordinary DOM API.
		const char* id = record.attribute("id").as_string();
		const char* text = record.child_value();

		std::cout << "record " << sink.count++ << ": id=" << id << " text=" << text << '\n';

		// To keep a record beyond the callback, copy it into a document the caller owns; the copy does
		// not reference the reader or any of its buffers:
		//
		// pugi::xml_document kept;
		// kept.append_copy(record);
	}
}

int main()
{
	// In a real program these chunks would arrive from a socket, a pipe or a streaming decoder.
	static const char chunks[][64] =
	{
		"<?xml version=\"1.0\"?><feed>",
		"<record id=\"1\">hello</record>",
		"<record id=\"2\">world</record>",
		"</feed>"
	};

	record_sink sink;
	pugi::xml_record_reader reader(on_record, &sink);

	for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i)
	{
		bool final = (i + 1 == sizeof(chunks) / sizeof(chunks[0]));
		pugi::record_read_result result = reader.feed(chunks[i], std::strlen(chunks[i]), final);

		if (result.status == pugi::error)
		{
			std::cerr << "parse error " << result.error << " at byte " << result.offset << '\n';
			return 1;
		}
	}

	std::cout << "processed " << sink.count << " records\n";
	return 0;
}

// vim:et
