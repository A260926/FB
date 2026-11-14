#include "record_reader.hpp"

// Bridge to the streaming engine implemented inside src/pugixml.cpp. The engine type is internal to
// pugixml's implementation namespace and crosses this boundary as an opaque pointer.
namespace pugi
{
	void* xml_record_reader_create_engine(xml_document& doc, void (*callback)(const xml_document&, void*), void* userdata);
	void xml_record_reader_destroy_engine(void* engine);
	int xml_record_reader_feed_engine(void* engine, const void* data, size_t size, bool final, xml_parse_status& error, size_t& offset);
	size_t xml_record_reader_retained_engine(const void* engine);
}

namespace pugi
{
	xml_record_reader::xml_record_reader(record_callback callback, void* userdata)
		: _document()
		, _engine(xml_record_reader_create_engine(_document, callback, userdata))
	{
	}

	xml_record_reader::~xml_record_reader()
	{
		xml_record_reader_destroy_engine(_engine);
	}

	record_read_result xml_record_reader::feed(const void* data, size_t size, bool final)
	{
		xml_parse_status error = status_ok;
		size_t offset = 0;

		int code = xml_record_reader_feed_engine(_engine, data, size, final, error, offset);

		record_read_result result;
		result.error = error;
		result.offset = offset;

		switch (code)
		{
		case 1:
			result.status = done;
			break;
		case 2:
			result.status = pugi::error;
			break;
		default:
			result.status = need_input;
			break;
		}

		return result;
	}

	size_t xml_record_reader::retained_input_bytes() const
	{
		return xml_record_reader_retained_engine(_engine);
	}
}
