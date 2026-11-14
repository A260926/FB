#ifndef HEADER_EXTRAS_RECORD_READER_HPP
#define HEADER_EXTRAS_RECORD_READER_HPP

// Incremental, record-at-a-time XML feed reader.
//
// Include this header together with the pugixml implementation (src/pugixml.cpp) and link the
// accompanying extras/record_reader.cpp. It supports the ordinary narrow char_t configuration only:
// the input is a stream of UTF-8 bytes, optionally prefixed with a UTF-8 BOM. wchar_t mode and other
// input encodings are not supported.

#include "../src/pugixml.hpp"

#include <cstddef>

namespace pugi
{
	// Outcome of a single xml_record_reader::feed call.
	enum record_read_status
	{
		need_input, // the chunk was consumed without completing the document; feed more bytes
		done,       // the final chunk was fed and the single root element is complete and closed
		error       // parsing failed; every subsequent feed returns the same result unchanged
	};

	// Result bundle of xml_record_reader::feed.
	// On success (need_input/done): error == status_ok and offset == 0.
	// On failure: error carries a pugixml xml_parse_status and offset is the absolute, zero-based byte
	// position of the fault in the concatenation of every buffer ever fed.
	struct record_read_result
	{
		record_read_status status;
		xml_parse_status error;
		size_t offset;
	};

	// Parses a single-root XML feed incrementally and delivers each direct child element of the root
	// (a "record") to a callback the moment its closing tag is parsed, including empty elements.
	//
	// Input model
	// ------------
	// The accepted document is one root element whose interior may contain whitespace, comments and
	// processing instructions plus any number of arbitrarily named direct child elements. Non-whitespace
	// character data directly inside the root is rejected with status_bad_pcdata. An XML declaration is
	// accepted only with version="1.0" and an omitted or UTF-8 encoding; a DOCTYPE declaration is
	// rejected with status_bad_doctype (no external resources are ever loaded). Record contents use the
	// same acceptance rules and conversions (entities, character references, CDATA, line endings) as
	// xml_document::load with parse_default.
	//
	// Lifecycle
	// ---------
	// Construct the reader with a callback and an opaque user pointer. Then call feed() repeatedly as
	// bytes arrive. A feed() that ends mid-token (a split tag name, attribute quote, character reference,
	// CRLF, UTF-8 sequence, comment or CDATA terminator) simply returns need_input and resumes from the
	// saved state on the next call; a chunk tail is never mistaken for the end of the document. A
	// zero-length, non-final feed changes no state. Once feed() returns, the caller may immediately
	// reuse or free the memory it passed in: the reader copies the bytes it still needs.
	//
	// The callback receives a temporary xml_document whose document element is the root (with all of its
	// attributes, e.g. xmlns, copied verbatim - no namespace processing is added) and whose sole child is
	// the just-completed record. The full DOM and XPath APIs are usable during the callback; all handles
	// into that document are invalid as soon as the callback returns. To keep a record, call
	// xml_document::append_copy (or append the relevant nodes) into a document the caller owns; the copy
	// is independent of the reader, its staging buffers and any future input.
	//
	// The reader is not copyable and provides no reentrancy or concurrency guarantees: do not call feed
	// (or touch the temporary document) from inside another feed/callback on the same reader, and do not
	// call feed concurrently from several threads.
	//
	// Error and offset rules
	// ----------------------
	// The first failure is sticky: later feed calls return the identical status and offset. Already
	// delivered records are not rolled back; the record that failed (and any unclosed one) is never
	// delivered. Offsets are byte offsets in the original, concatenated input stream and are unaffected
	// by UTF-8 decoding, CRLF folding or the fact that consumed prefixes are released:
	//   - a mismatched closing tag reports status_end_element_mismatch at the '<' of that closing tag;
	//   - an unclosed structure on the final feed reports status_end_element_mismatch at the total number
	//     of bytes fed (a document with no element reports status_no_document_element);
	//   - other syntax errors point at the start of the offending token, text errors at the start of the
	//     offending text run;
	//   - a second element after the closed root reports status_bad_start_element.
	class PUGIXML_CLASS xml_record_reader
	{
	public:
		typedef void (*record_callback)(const xml_document& doc, void* userdata);

		explicit xml_record_reader(record_callback callback, void* userdata = 0);
		~xml_record_reader();

		// Non-copyable. The reader is a single-use, single-threaded streaming object.
		xml_record_reader(const xml_record_reader&) = delete;
		xml_record_reader& operator=(const xml_record_reader&) = delete;

		// Feeds the next contiguous run of UTF-8 bytes. With final == false a partial token is parked and
		// need_input is returned. With final == true the document must be complete: done is returned when
		// the root element is closed, otherwise an error result is returned.
		record_read_result feed(const void* data, size_t size, bool final = false);

		// Total capacity of the raw-input byte storage the reader currently owns: the input block plus the
		// staging space of any unfinished token. This counts allocated capacity (not merely unconsumed
		// length) and excludes copies the callback made.
		size_t retained_input_bytes() const;

	private:
		xml_document _document;
		void* _engine;
	};
}

#endif
