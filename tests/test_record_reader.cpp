#include "test.hpp"
#include "writer_string.hpp"

#include "../extras/record_reader.hpp"

#include <cstring>
#include <string>
#include <vector>

// The check-target build links a fixed, hand-listed set of translation units (see
// baseline/check-target.sh) that does not compile the shared helper TUs tests/allocator.cpp and
// tests/writer_string.cpp. Include them here so the harness symbols (memory_allocate/memory_size/
// memory_deallocate and xml_writer_string) resolve for this translation unit.
#include "allocator.cpp"
#include "writer_string.cpp"

namespace
{
	// Serialize a node the same way the bulk reference and the streaming results are compared.
	std::string serialize(const pugi::xml_node& node)
	{
		xml_writer_string writer;
		node.print(writer, STR(""), pugi::format_raw);
		return writer.as_narrow();
	}

	struct collected_record
	{
		std::string name;
		std::string serialized;
		std::string id;
		std::string text;
	};

	struct collector
	{
		std::vector<collected_record> records;
		std::vector<std::string> root_attributes;
		pugi::xml_document* keeper; // optional: append_copy of the first record lands here
		size_t records_seen_in_callback;

		collector(): keeper(0), records_seen_in_callback(0)
		{
		}

		void operator()(const pugi::xml_document& doc)
		{
			pugi::xml_node root = doc.document_element();

			if (records.empty())
			{
				for (pugi::xml_attribute a = root.first_attribute(); a; a = a.next_attribute())
				{
					root_attributes.push_back(std::string(a.name()) + "=" + a.value());
				}
			}

			// every callback document contains exactly one record under the root
			pugi::xml_node record = root.first_child();
			CHECK(record);
			CHECK(!record.next_sibling());

			collected_record cr;
			cr.name = record.name();
			cr.serialized = serialize(record);
			cr.id = record.attribute("id").value();
			cr.text = record.child_value();
			records.push_back(cr);

			++records_seen_in_callback;

			if (keeper && records_seen_in_callback == 1)
			{
				// an independent copy must not depend on the reader or its buffers
				keeper->append_copy(record);
			}
		}
	};

	void record_callback(const pugi::xml_document& doc, void* userdata)
	{
		(*static_cast<collector*>(userdata))(doc);
	}

	// Reference: bulk parse_default, returning the serialized element records in order.
	std::vector<std::string> reference_records(const std::string& xml)
	{
		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default);
		CHECK(result);

		std::vector<std::string> out;
		for (pugi::xml_node node = doc.document_element().first_child(); node; node = node.next_sibling())
		{
			if (node.type() == pugi::node_element)
				out.push_back(serialize(node));
		}
		return out;
	}

	std::vector<std::string> reference_root_attributes(const std::string& xml)
	{
		pugi::xml_document doc;
		CHECK(doc.load_buffer(xml.data(), xml.size(), pugi::parse_default));

		std::vector<std::string> out;
		for (pugi::xml_attribute a = doc.document_element().first_attribute(); a; a = a.next_attribute())
			out.push_back(std::string(a.name()) + "=" + a.value());
		return out;
	}

	// Feeds the whole input using a fixed chunk size, optionally interleaving empty blocks and reusing
	// a caller-owned scratch buffer that is clobbered after every feed.
	pugi::record_read_result feed_chunked(pugi::xml_record_reader& reader, const std::string& xml,
		size_t chunk, bool interleave_empty, bool reuse_buffer)
	{
		std::string scratch;
		pugi::record_read_result result;
		size_t pos = 0;
		unsigned long counter = 0;

		while (pos < xml.size())
		{
			size_t n = chunk < xml.size() - pos ? chunk : xml.size() - pos;

			if (interleave_empty && (counter++ % 3 == 1))
			{
				// a zero-length, non-final feed must leave every state untouched
				result = reader.feed("", 0, false);
				CHECK(result.status == pugi::need_input);
				CHECK(result.error == pugi::status_ok);
				CHECK(result.offset == 0);
			}

			const void* data;
			if (reuse_buffer)
			{
				scratch.assign(xml.data() + pos, n);
				data = scratch.data();
			}
			else
			{
				data = xml.data() + pos;
			}

			bool last = (pos + n == xml.size());
			result = reader.feed(data, n, last);

			if (reuse_buffer)
				std::memset(scratch.data(), 'Z', scratch.size()); // memory may be overwritten immediately

			if (result.status == pugi::error)
				return result;

			pos += n;
		}

		if (result.status != pugi::done)
			result = reader.feed("", 0, true);

		return result;
	}

	// Two-feed split at every possible single cut position.
	pugi::record_read_result feed_single_cut(pugi::xml_record_reader& reader, const std::string& xml, size_t cut)
	{
		pugi::record_read_result r;

		if (cut > 0)
		{
			r = reader.feed(xml.data(), cut, false);
			if (r.status == pugi::error) return r;
		}

		return reader.feed(xml.data() + cut, xml.size() - cut, true);
	}

	// Recursive bisection: the stream is partitioned by repeatedly halving intervals, so every internal
	// bisection boundary of the input is exercised as a feed edge.
	void bisection_cuts(const std::string& xml, size_t begin, size_t end, std::vector<size_t>& cuts)
	{
		if (end - begin < 2) return;

		size_t mid = begin + (end - begin) / 2;
		cuts.push_back(mid);
		bisection_cuts(xml, begin, mid, cuts);
		bisection_cuts(xml, mid, end, cuts);
	}

	pugi::record_read_result feed_cut_list(pugi::xml_record_reader& reader, const std::string& xml,
		const std::vector<size_t>& cuts)
	{
		std::vector<size_t> bounds = cuts;
		bounds.push_back(0);
		bounds.push_back(xml.size());

		// order the boundaries
		for (size_t i = 0; i < bounds.size(); ++i)
			for (size_t j = i + 1; j < bounds.size(); ++j)
				if (bounds[j] < bounds[i])
				{
					size_t tmp = bounds[i];
					bounds[i] = bounds[j];
					bounds[j] = tmp;
				}

		pugi::record_read_result result;
		for (size_t i = 1; i < bounds.size(); ++i)
		{
			size_t begin = bounds[i - 1];
			size_t end = bounds[i];
			bool last = (end == xml.size());
			result = reader.feed(xml.data() + begin, end - begin, last);
			if (result.status == pugi::error) return result;
		}

		if (result.status != pugi::done)
			result = reader.feed("", 0, true);
		return result;
	}

	const char* sample_feed()
	{
		return
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
			"<feed xmlns=\"http://example.test/ns\" version=\"42\" region=\"eu-west\">"
			"<!-- header comment -->"
			"<item id=\"a\" kind=\"first\">plain &#x1F600; &amp; more\r\n"
			"<![CDATA[<raw cdata>]]><leaf attr=\"v\">deep</leaf></item>"
			"<item id='b'>second &quot;quote&quot;</item>"
			"<item id=\"c\"><nested><deeply>x</deeply></nested></item>"
			"<empty id=\"e\"/>"
			"<?processing instruction?>"
			"</feed>";
	}
}

TEST(record_reader_content)
{
	const std::string xml = sample_feed();

	// reference built from a single bulk parse_default
	std::vector<std::string> expected = reference_records(xml);
	std::vector<std::string> expected_root = reference_root_attributes(xml);
	CHECK(expected.size() == 4);

	collector col;
	pugi::xml_record_reader reader(record_callback, &col);
	pugi::record_read_result result = reader.feed(xml.data(), xml.size(), true);

	CHECK(result.status == pugi::done);
	CHECK(result.error == pugi::status_ok);
	CHECK(result.offset == 0);

	// root attributes (including xmlns) are copied verbatim and visible during every callback
	CHECK(col.root_attributes == expected_root);

	CHECK(col.records.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i)
		CHECK(col.records[i].serialized == expected[i]);

	// spot-check content kinds
	CHECK(col.records[0].id == "a");
	CHECK(col.records[0].text.find('\n') != std::string::npos); // CRLF folded to LF
	CHECK(col.records[3].name == "empty");

	// the same records must come out byte-identical when fed as 17-byte blocks
	collector chunked;
	pugi::xml_record_reader chunked_reader(record_callback, &chunked);
	pugi::record_read_result chunked_result =
		feed_chunked(chunked_reader, xml, 17, true, true);

	CHECK(chunked_result.status == pugi::done);
	CHECK(chunked.records.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i)
		CHECK(chunked.records[i].serialized == expected[i]);
}

TEST(record_reader_chunk_edges)
{
	const std::string xml = sample_feed();
	std::vector<std::string> expected = reference_records(xml);

	// byte-at-a-time, with empty blocks and reused/overwritten scratch storage
	{
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = feed_chunked(reader, xml, 1, true, true);

		CHECK(result.status == pugi::done);
		CHECK(col.records.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(col.records[i].serialized == expected[i]);
	}

	// fixed 17-byte blocks
	{
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = feed_chunked(reader, xml, 17, false, false);

		CHECK(result.status == pugi::done);
		CHECK(col.records.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(col.records[i].serialized == expected[i]);
	}

	// every single cut position
	for (size_t cut = 0; cut <= xml.size(); ++cut)
	{
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = feed_single_cut(reader, xml, cut);

		CHECK(result.status == pugi::done);
		CHECK(col.records.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(col.records[i].serialized == expected[i]);
	}

	// every bisection boundary
	{
		std::vector<size_t> cuts;
		bisection_cuts(xml, 0, xml.size(), cuts);
		CHECK(!cuts.empty());

		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = feed_cut_list(reader, xml, cuts);

		CHECK(result.status == pugi::done);
		CHECK(col.records.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(col.records[i].serialized == expected[i]);
	}

	// a leading UTF-8 BOM must be accepted and produce the same records
	{
		std::string with_bom = std::string("\xef\xbb\xbf", 3) + xml;
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = feed_chunked(reader, with_bom, 13, true, true);

		CHECK(result.status == pugi::done);
		CHECK(col.records.size() == expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
			CHECK(col.records[i].serialized == expected[i]);
	}
}

TEST(record_reader_delivery)
{
	const std::string xml =
		"<feed><item id=\"one\">first</item><item id=\"two\">second</item><item id=\"three\">third</item></feed>";

	pugi::xml_document keeper;
	collector col;
	col.keeper = &keeper;

	pugi::record_read_result final_result;
	{
		pugi::xml_record_reader reader(record_callback, &col);

		// feed everything except the root closing tag so the root stays open
		const size_t tail = std::strlen("</feed>");
		CHECK(xml.compare(xml.size() - tail, tail, "</feed>") == 0);

		pugi::record_read_result partial = reader.feed(xml.data(), xml.size() - tail, false);
		CHECK(partial.status == pugi::need_input);

		// while the root is still open, all earlier records have already been delivered
		CHECK(col.records_seen_in_callback == 3);

		// the first record was copied out independently while still inside its callback
		pugi::xml_node kept = keeper.first_child();
		CHECK(kept);
		CHECK_STRING(kept.attribute("id").value(), STR("one"));
		CHECK_STRING(kept.child_value(), STR("first"));

		final_result = reader.feed(xml.data() + xml.size() - tail, tail, true);
		CHECK(final_result.status == pugi::done);
	}
	// reader is destroyed here; the kept copy must survive on its own storage

	pugi::xml_node kept = keeper.first_child();
	CHECK(kept);
	CHECK_STRING(kept.attribute("id").value(), STR("one"));
	CHECK_STRING(kept.child_value(), STR("first"));
	CHECK_NODE(kept, STR("<item id=\"one\">first</item>"));

	// delivery order and content
	CHECK(col.records.size() == 3);
	CHECK(col.records[0].id == "one");
	CHECK(col.records[1].id == "two");
	CHECK(col.records[2].id == "three");
}

TEST(record_reader_errors)
{
	// the first record (multibyte text + character reference) is delivered, then the second mismatches
	const std::string mismatch =
		"<feed><item id=\"ok\">\xE4\xBD\xA0 &#65; &#x42;</item><item id=\"bad\">x</wrong></item></feed>";

	const size_t mismatch_offset = mismatch.find("</wrong>");
	CHECK(mismatch_offset != std::string::npos);

	pugi::record_read_status first_status = pugi::need_input;
	pugi::xml_parse_status first_error = pugi::status_ok;
	size_t first_offset = 0;
	size_t first_deliveries = 0;

	for (size_t chunk = 1; chunk <= mismatch.size(); ++chunk)
	{
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = feed_chunked(reader, mismatch, chunk, chunk % 5 == 0, true);

		CHECK(result.status == pugi::error);
		CHECK(result.error == pugi::status_end_element_mismatch);
		CHECK(result.offset == mismatch_offset); // absolute position of the offending '<'

		// the good first record is delivered; the bad, unclosed one is never delivered
		CHECK(col.records.size() == 1);
		CHECK(col.records[0].id == "ok");

		if (chunk == 1)
		{
			first_status = result.status;
			first_error = result.error;
			first_offset = result.offset;
			first_deliveries = col.records.size();
		}
		else
		{
			CHECK(result.status == first_status);
			CHECK(result.error == first_error);
			CHECK(result.offset == first_offset);
			CHECK(col.records.size() == first_deliveries);
		}
	}

	// failures are sticky: later feeds keep returning the first error and offset
	{
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result r1 = reader.feed(mismatch.data(), mismatch.size(), false);
		CHECK(r1.status == pugi::error);
		CHECK(r1.error == pugi::status_end_element_mismatch);
		CHECK(r1.offset == mismatch_offset);

		pugi::record_read_result r2 = reader.feed("<whatever>", 10, false);
		CHECK(r2.status == pugi::error);
		CHECK(r2.error == r1.error);
		CHECK(r2.offset == r1.offset);

		pugi::record_read_result r3 = reader.feed("", 0, true);
		CHECK(r3.status == pugi::error);
		CHECK(r3.error == r1.error);
		CHECK(r3.offset == r1.offset);
	}

	// final feed with unclosed structures: offset is the total number of bytes fed
	{
		const char* truncated[] =
		{
			"<feed><item>unclosed",
			"<feed><item><nested/></item",
			"<feed><item attr='value"
		};

		for (size_t i = 0; i < sizeof(truncated) / sizeof(truncated[0]); ++i)
		{
			std::string xml = truncated[i];
			collector col;
			pugi::xml_record_reader reader(record_callback, &col);
			pugi::record_read_result result = reader.feed(xml.data(), xml.size(), true);

			CHECK(result.status == pugi::error);
			CHECK(result.error == pugi::status_end_element_mismatch);
			CHECK(result.offset == xml.size());
			CHECK(col.records.empty());
		}
	}

	// DOCTYPE is rejected at its opening declaration
	{
		const std::string xml = "<feed><a/></feed><!DOCTYPE feed>";
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		// place the DOCTYPE where a parser would first encounter it (before the root here)
		const std::string doctype_first = "<!DOCTYPE feed><feed><a/></feed>";
		pugi::record_read_result result = reader.feed(doctype_first.data(), doctype_first.size(), true);

		CHECK(result.status == pugi::error);
		CHECK(result.error == pugi::status_bad_doctype);
		CHECK(result.offset == 0);
		(void)xml;
	}

	// a second element after the closed root is status_bad_start_element at the offending '<'
	{
		const std::string xml = "<feed><a/></feed><second/>";
		const size_t second_offset = xml.find("<second/>");
		CHECK(second_offset != std::string::npos);

		for (size_t chunk = 1; chunk <= xml.size(); ++chunk)
		{
			collector col;
			pugi::xml_record_reader reader(record_callback, &col);
			pugi::record_read_result result = feed_chunked(reader, xml, chunk, false, false);

			CHECK(result.status == pugi::error);
			CHECK(result.error == pugi::status_bad_start_element);
			CHECK(result.offset == second_offset);
			CHECK(col.records.size() == 1);
		}
	}

	// non-whitespace text directly inside the root is status_bad_pcdata at the text start
	{
		const std::string xml = "<feed>bad text<a/></feed>";
		collector col;
		pugi::xml_record_reader reader(record_callback, &col);
		pugi::record_read_result result = reader.feed(xml.data(), xml.size(), true);

		CHECK(result.status == pugi::error);
		CHECK(result.error == pugi::status_bad_pcdata);
		CHECK(result.offset == 6);
	}
}

namespace
{
	struct tracking_header
	{
		size_t size;
	};

	size_t g_live_bytes = 0;
	size_t g_peak_bytes = 0;

	void* tracking_allocate(size_t size)
	{
		tracking_header* header = static_cast<tracking_header*>(std::malloc(sizeof(tracking_header) + size));
		if (!header) return 0;
		header->size = size;
		g_live_bytes += size;
		if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
		return header + 1;
	}

	void tracking_deallocate(void* ptr)
	{
		if (!ptr) return;
		tracking_header* header = static_cast<tracking_header*>(ptr) - 1;
		g_live_bytes -= header->size;
		std::free(header);
	}
}

TEST(record_reader_memory)
{
	const size_t record_count = 100000;
	const size_t chunk_size = 31;
	const size_t limit = 256 * 1024;

	pugi::allocation_function old_allocate = pugi::get_memory_allocation_function();
	pugi::deallocation_function old_deallocate = pugi::get_memory_deallocation_function();

	g_live_bytes = 0;
	g_peak_bytes = 0;
	pugi::set_memory_management_functions(tracking_allocate, tracking_deallocate);

	size_t max_retained = 0;
	bool under_limit = true;
	{
		collector col;
		// the callback keeps no copies; drop everything it collects
		pugi::xml_record_reader reader(
			[](const pugi::xml_document& doc, void* userdata)
			{
				collector* c = static_cast<collector*>(userdata);
				(*c)(doc);
				c->records.clear();
				c->root_attributes.clear();
			},
			&col);

		const std::string header = "<feed>";
		const std::string footer = "</feed>";

		pugi::record_read_result result = reader.feed(header.data(), header.size(), false);
		under_limit = under_limit && result.status == pugi::need_input;

		// generate and stream the corpus on the fly; the full feed is never materialized
		std::string row;
		for (size_t i = 0; i < record_count; ++i)
		{
			row.clear();
			row += "<item n=\"";
			row += std::to_string(i);
			row += "\">";
			row += std::string(220, 'x'); // makes each record roughly 256 bytes
			row += "</item>";
			CHECK(row.size() >= 235 && row.size() <= 256);

			size_t pos = 0;
			while (pos < row.size())
			{
				size_t n = chunk_size < row.size() - pos ? chunk_size : row.size() - pos;
				result = reader.feed(row.data() + pos, n, false);
				under_limit = under_limit && result.status == pugi::need_input;

				size_t retained = reader.retained_input_bytes();
				if (retained > max_retained) max_retained = retained;
				under_limit = under_limit && retained <= limit;

				pos += n;
			}
		}

		result = reader.feed(footer.data(), footer.size(), true);
		under_limit = under_limit && result.status == pugi::done;
		under_limit = under_limit && reader.retained_input_bytes() <= limit;
	}

	const size_t peak = g_peak_bytes;
	pugi::set_memory_management_functions(old_allocate, old_deallocate);

	CHECK(under_limit);
	CHECK(max_retained <= limit);
	CHECK(peak <= limit);
	CHECK(g_live_bytes == 0);
}
