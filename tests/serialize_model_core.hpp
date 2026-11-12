// Structured serialization model for pugixml.
//
// The model generates documents from arbitrary fuzz bytes using a bounded,
// deterministic mapping.  Values are stored in pugixml's native char_t
// encoding through the DOM API and are saved through xml_writer.  The
// expected output is built independently, scalar by scalar, and encoded to
// the requested XML byte encoding.  Only the fully concatenated writer
// bytes are compared; no assumption is made about writer chunking.

#ifndef SERIALIZE_MODEL_CORE_HPP
#define SERIALIZE_MODEL_CORE_HPP

#include "pugixml.hpp"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

namespace serialize_model
{
	typedef unsigned int scalar_t;

	// ---------- compile configuration ----------------------------------------

	inline bool host_little_endian()
	{
		unsigned int probe = 1;
		return *reinterpret_cast<unsigned char*>(&probe) == 1;
	}

	inline size_t output_stack_bytes()
	{
	#ifdef PUGIXML_MEMORY_OUTPUT_STACK
		return PUGIXML_MEMORY_OUTPUT_STACK;
	#else
		return 10240;
	#endif
	}

	inline size_t native_buffer_capacity()
	{
		return output_stack_bytes() / (sizeof(pugi::char_t) + 4);
	}

	inline const char* native_encoding_name()
	{
	#ifdef PUGIXML_WCHAR_MODE
		if (sizeof(wchar_t) == 2)
			return host_little_endian() ? "utf16_le" : "utf16_be";
		else
			return host_little_endian() ? "utf32_le" : "utf32_be";
	#else
		return "utf8";
	#endif
	}

	inline std::string config_id()
	{
		std::string result = "char_t=";
	#ifdef PUGIXML_WCHAR_MODE
		result += "wchar_t";
	#else
		result += "char";
	#endif
		result += ",output_stack=";
		result += std::to_string(output_stack_bytes());
		result += ",wchar_bytes=";
		result += std::to_string(sizeof(wchar_t));
		result += ",char_t_bytes=";
		result += std::to_string(sizeof(pugi::char_t));
		result += ",native=";
		result += native_encoding_name();
		return result;
	}

	// ---------- encoding ------------------------------------------------------

	struct OutputEncoding
	{
		pugi::xml_encoding value;
		const char* name;
	};

	inline const OutputEncoding* output_encodings(size_t& count)
	{
		static const OutputEncoding encodings[] =
		{
			{ pugi::encoding_utf8,     "utf8"      },
			{ pugi::encoding_utf16_le, "utf16_le"  },
			{ pugi::encoding_utf16_be, "utf16_be"  },
			{ pugi::encoding_utf32_le, "utf32_le"  },
			{ pugi::encoding_utf32_be, "utf32_be"  }
		};

		count = sizeof(encodings) / sizeof(encodings[0]);
		return encodings;
	}

	inline void append_utf8_byte(std::string& out, unsigned int value)
	{
		out.push_back(static_cast<char>(value & 0xff));
	}

	inline void append_utf8_scalar(std::string& out, scalar_t cp)
	{
		if (cp < 0x80)
		{
			append_utf8_byte(out, cp);
		}
		else if (cp < 0x800)
		{
			append_utf8_byte(out, 0xc0 | (cp >> 6));
			append_utf8_byte(out, 0x80 | (cp & 0x3f));
		}
		else if (cp < 0x10000)
		{
			append_utf8_byte(out, 0xe0 | (cp >> 12));
			append_utf8_byte(out, 0x80 | ((cp >> 6) & 0x3f));
			append_utf8_byte(out, 0x80 | (cp & 0x3f));
		}
		else
		{
			append_utf8_byte(out, 0xf0 | (cp >> 18));
			append_utf8_byte(out, 0x80 | ((cp >> 12) & 0x3f));
			append_utf8_byte(out, 0x80 | ((cp >> 6) & 0x3f));
			append_utf8_byte(out, 0x80 | (cp & 0x3f));
		}
	}

	// Convert a scalar sequence to the native representation used by pugixml
	// in this build.  This intentionally depends on sizeof(wchar_t) rather
	// than on a hard-coded representation of the build machine.  A vector is
	// used rather than std::basic_string because libc++ std::basic_string
	// has non-portable element handling when wchar_t is not the ABI type;
	// literals built as vectors are unaffected.
	inline std::vector<pugi::char_t> to_native(const std::vector<scalar_t>& scalars)
	{
		std::vector<pugi::char_t> result;

	#ifdef PUGIXML_WCHAR_MODE
		if (sizeof(pugi::char_t) == 2)
		{
			for (size_t i = 0; i < scalars.size(); ++i)
			{
				scalar_t cp = scalars[i];

				if (cp < 0x10000)
				{
					result.push_back(static_cast<pugi::char_t>(cp));
				}
				else
				{
					scalar_t rest = cp - 0x10000;
					result.push_back(static_cast<pugi::char_t>(0xd800 + (rest >> 10)));
					result.push_back(static_cast<pugi::char_t>(0xdc00 + (rest & 0x3ff)));
				}
			}
		}
		else
		{
			for (size_t i = 0; i < scalars.size(); ++i)
				result.push_back(static_cast<pugi::char_t>(scalars[i]));
		}
	#else
		std::string utf8;

		for (size_t i = 0; i < scalars.size(); ++i)
			append_utf8_scalar(utf8, scalars[i]);

		// Note: append one byte at a time; char_t is a narrow char here.
		for (size_t i = 0; i < utf8.size(); ++i)
			result.push_back(static_cast<pugi::char_t>(utf8[i]));
	#endif

		return result;
	}

	// Encode a scalar sequence to bytes in the requested XML encoding.  This
	// is a complete little reference implementation, not a pugixml call.
	// When byte_to_scalar is non-null, each produced output byte is mapped to
	// the index of the scalar that produced it.
	inline std::string encode_scalars(const std::vector<scalar_t>& scalars, pugi::xml_encoding encoding, std::vector<size_t>* byte_to_scalar)
	{
		std::string out;

		if (byte_to_scalar)
			byte_to_scalar->clear();

		auto put8 = [&](unsigned int value, size_t scalar_index)
		{
			out.push_back(static_cast<char>(value & 0xff));
			if (byte_to_scalar) byte_to_scalar->push_back(scalar_index);
		};

		auto put16 = [&](uint16_t value, size_t scalar_index)
		{
			bool le = (encoding == pugi::encoding_utf16_le);

			if (le)
			{
				put8(value & 0xff, scalar_index);
				put8(value >> 8, scalar_index);
			}
			else
			{
				put8(value >> 8, scalar_index);
				put8(value & 0xff, scalar_index);
			}
		};

		auto put32 = [&](uint32_t value, size_t scalar_index)
		{
			bool le = (encoding == pugi::encoding_utf32_le);

			if (le)
			{
				put8(value & 0xff, scalar_index);
				put8((value >> 8) & 0xff, scalar_index);
				put8((value >> 16) & 0xff, scalar_index);
				put8((value >> 24) & 0xff, scalar_index);
			}
			else
			{
				put8((value >> 24) & 0xff, scalar_index);
				put8((value >> 16) & 0xff, scalar_index);
				put8((value >> 8) & 0xff, scalar_index);
				put8(value & 0xff, scalar_index);
			}
		};

		for (size_t i = 0; i < scalars.size(); ++i)
		{
			scalar_t cp = scalars[i];

			if (encoding == pugi::encoding_utf8)
			{
				if (cp < 0x80)
					put8(cp, i);
				else if (cp < 0x800)
				{
					put8(0xc0 | (cp >> 6), i);
					put8(0x80 | (cp & 0x3f), i);
				}
				else if (cp < 0x10000)
				{
					put8(0xe0 | (cp >> 12), i);
					put8(0x80 | ((cp >> 6) & 0x3f), i);
					put8(0x80 | (cp & 0x3f), i);
				}
				else
				{
					put8(0xf0 | (cp >> 18), i);
					put8(0x80 | ((cp >> 12) & 0x3f), i);
					put8(0x80 | ((cp >> 6) & 0x3f), i);
					put8(0x80 | (cp & 0x3f), i);
				}
			}
			else if (encoding == pugi::encoding_utf16_le || encoding == pugi::encoding_utf16_be)
			{
				if (cp < 0x10000)
				{
					put16(static_cast<uint16_t>(cp), i);
				}
				else
				{
					scalar_t rest = cp - 0x10000;
					put16(static_cast<uint16_t>(0xd800 + (rest >> 10)), i);
					put16(static_cast<uint16_t>(0xdc00 + (rest & 0x3ff)), i);
				}
			}
			else if (encoding == pugi::encoding_utf32_le || encoding == pugi::encoding_utf32_be)
			{
				put32(cp, i);
			}
			else
			{
				// The model only asks for the five explicit UTF encodings.
				put8('?', i);
			}
		}

		return out;
	}

	// ---------- model ---------------------------------------------------------

	struct CaseSpec
	{
		std::string name;

		// Original fuzz bytes; retained so a failure can print an exact,
		// directly replayable hexadecimal input without building the hex
		// string on every successful case.
		const uint8_t* input_data;
		size_t input_size;

		std::vector<scalar_t> attr[2];
		std::vector<scalar_t> pcdata;
		std::vector<scalar_t> cdata;

		bool test_no_escapes;

		CaseSpec(): name(), input_data(0), input_size(0), test_no_escapes(false)
		{
		}
	};

	// Valid XML 1.0 scalar values used by the generator.  The set covers
	// every UTF-8 length class, both sides of the BMP boundary, and the
	// minimum / a large supplementary-plane scalar.
	inline const scalar_t* palette_scalars(size_t& count)
	{
		static const scalar_t palette[] =
		{
			'A', 'B', 'C', 'z', '0', ' ',      // 1-byte UTF-8
			0x0080, 0x00a9, 0x00e9, 0x07ff,    // 2-byte UTF-8
			0x0800, 0x4e2d, 0xd7ff, 0xe000, 0xfffd, // 3-byte UTF-8, BMP edges
			0x10000, 0x1f642, 0x10ffff        // 4-byte UTF-8, above BMP / minimum / maximum
		};

		count = sizeof(palette) / sizeof(palette[0]);
		return palette;
	}

	inline const scalar_t* special_scalars(size_t& count)
	{
		static const scalar_t specials[] = { '&', '<', '>', '"', '\'' };
		count = sizeof(specials) / sizeof(specials[0]);
		return specials;
	}

	inline bool scalar_is_xml_special(scalar_t cp)
	{
		return cp == '&' || cp == '<' || cp == '>' || cp == '"' || cp == '\'';
	}

	inline bool is_clean_text(const std::vector<scalar_t>& value)
	{
		for (size_t i = 0; i < value.size(); ++i)
		{
			scalar_t cp = value[i];

			if (scalar_is_xml_special(cp)) return false;
			if (cp < 0x20) return false;
		}

		return true;
	}

	inline bool case_is_clean(const CaseSpec& spec)
	{
		return is_clean_text(spec.attr[0]) && is_clean_text(spec.attr[1]) && is_clean_text(spec.pcdata);
	}

	inline uint64_t fnv1a_hash(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ULL;
		size_t limit = size < 4096 ? size : 4096;

		for (size_t i = 0; i < limit; ++i)
		{
			hash ^= static_cast<uint64_t>(data[i]);
			hash *= 1099511628211ULL;
		}

		// Fold the full size in so that different-length inputs remain distinct.
		hash ^= static_cast<uint64_t>(size);
		hash *= 1099511628211ULL;

		return hash;
	}

	struct Rng
	{
		uint64_t state;

		explicit Rng(uint64_t seed): state(seed ? seed : 0x9e3779b97f4a7c15ULL)
		{
		}

		uint64_t next()
		{
			uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
			return z ^ (z >> 31);
		}

		size_t below(size_t n)
		{
			return n ? static_cast<size_t>(next() % n) : 0;
		}
	};

	inline size_t fixed_family_count()
	{
		return 11;
	}

	inline const char* fixed_family_name(size_t family)
	{
		static const char* names[] =
		{
			"fixed.align.pcdata_boundary",
			"fixed.align.attr_boundary",
			"fixed.align.cdata_boundary",
			"fixed.supplementary_run_pcdata",
			"fixed.ascii_multibyte_alternating",
			"fixed.escape_adjacent_multibyte",
			"fixed.attr_quote_matrix",
			"fixed.cdata_end_split",
			"fixed.long_values_over_buffer",
			"fixed.no_escapes_text",
			"fixed.no_escapes_long"
		};

		return family < fixed_family_count() ? names[family] : "fixed.unknown";
	}

	inline bool detect_fixed_family(const uint8_t* data, size_t size, size_t& family)
	{
		// Deliberately improbable magic so arbitrary fuzz bytes normally take
		// the random-model path, while a tiny deterministic input can replay
		// one of the fixed families through the same fuzz entry.
		if (size >= 4 && data[0] == 'S' && data[1] == 'M' && data[2] == 'F' && data[3] < fixed_family_count())
		{
			family = static_cast<size_t>(data[3]);
			return true;
		}

		return false;
	}

	inline std::string bytes_to_hex(const uint8_t* data, size_t size)
	{
		std::string out;
		out.reserve(size * 2);

		for (size_t i = 0; i < size; ++i)
		{
			char buffer[8];
			snprintf(buffer, sizeof(buffer), "%02x", static_cast<unsigned int>(data[i]));
			out += buffer;
		}

		return out;
	}

	inline std::vector<scalar_t> make_long_value(size_t target_length)
	{
		static const scalar_t seed_sequence[] =
		{
			'A', 0x00a9, 0x07ff, 0x0800, 0x4e2d, 0xfffd, 0x10000, 0x1f642, 0x10ffff
		};

		std::vector<scalar_t> value;

		for (size_t i = 0; i < target_length; ++i)
			value.push_back(seed_sequence[i % (sizeof(seed_sequence) / sizeof(seed_sequence[0]))]);

		if (value.empty())
			value.push_back('A');

		return value;
	}

	// Build a value whose first multibyte scalar straddles the native output
	// buffer boundary.  The exact layout depends on the native character size
	// and is derived from sizeof(pugi::char_t), never hard-coded:
	//   * narrow (1-byte char_t): UTF-8 lead byte two units before the
	//     boundary, so a three-byte scalar extends past it;
	//   * wide 2-byte char_t: the lead surrogate sits on the last in-buffer
	//     unit, with the low surrogate just after the boundary;
	//   * wide 4-byte char_t: a supplementary scalar sits on the last
	//     in-buffer unit.
	// The trailing tail makes write_direct take the chunked conversion path.
	inline std::vector<scalar_t> make_boundary_value()
	{
		size_t capacity = native_buffer_capacity();
		size_t filler;

		if (sizeof(pugi::char_t) == 1)
			filler = capacity > 2 ? capacity - 2 : 1;
		else
			filler = capacity > 1 ? capacity - 1 : 1;

		std::vector<scalar_t> value;

		for (size_t i = 0; i < filler; ++i)
			value.push_back(static_cast<scalar_t>('A' + (i % 26)));

		if (sizeof(pugi::char_t) == 2)
		{
			value.push_back(0x1f642);
			value.push_back(0x4e2d);
		}
		else if (sizeof(pugi::char_t) == 4)
		{
			value.push_back(0x1f642);
			value.push_back(0x4e2d);
		}
		else
		{
			// Narrow UTF-8: the three-byte scalar straddles the boundary,
			// followed by a four-byte scalar on the other side.
			value.push_back(0x4e2d);
			value.push_back(0x1f642);
		}

		for (size_t i = 0; i < 8; ++i)
			value.push_back(static_cast<scalar_t>('B' + (i % 26)));

		return value;
	}

	inline CaseSpec make_fixed_case(size_t family)
	{
		CaseSpec spec;

		spec.name = fixed_family_name(family);
		spec.attr[0].assign(1, 'x');
		spec.attr[1].assign(1, 'y');
		spec.pcdata.assign(1, 'p');
		spec.cdata.assign(1, 'c');

		switch (family)
		{
		case 0:
			spec.pcdata = make_boundary_value();
			break;

		case 1:
			spec.attr[0] = make_boundary_value();
			break;

		case 2:
			spec.cdata = make_boundary_value();
			break;

		case 3:
			spec.pcdata.clear();
			for (size_t i = 0; i < 6; ++i)
			{
				spec.pcdata.push_back(0x10000);
				spec.pcdata.push_back(0x1f642);
				spec.pcdata.push_back(0x10ffff);
			}
			break;

		case 4:
			spec.pcdata.clear();
			for (size_t i = 0; i < 8; ++i)
			{
				spec.pcdata.push_back('A');
				spec.pcdata.push_back(0x00e9);
				spec.pcdata.push_back('B');
				spec.pcdata.push_back(0x4e2d);
				spec.pcdata.push_back('C');
				spec.pcdata.push_back(0x1f642);
			}
			break;

		case 5:
			spec.pcdata.clear();
			spec.pcdata.push_back(0x1f642);
			spec.pcdata.push_back('&');
			spec.pcdata.push_back(0x4e2d);
			spec.pcdata.push_back('<');
			spec.pcdata.push_back(0x10000);
			spec.pcdata.push_back('>');
			spec.pcdata.push_back(0x4e2d);
			spec.pcdata.push_back('"');
			spec.pcdata.push_back(0x1f642);
			spec.pcdata.push_back('\'');

			spec.attr[0].clear();
			spec.attr[0].push_back('&');
			spec.attr[0].push_back(0x1f642);
			spec.attr[0].push_back('<');
			spec.attr[0].push_back('"');
			spec.attr[0].push_back(0x4e2d);
			spec.attr[0].push_back('\'');

			spec.attr[1].clear();
			spec.attr[1].push_back(0x10000);
			spec.attr[1].push_back('&');
			spec.attr[1].push_back(0x4e2d);
			// both quote characters are present to exercise both formats
			spec.attr[1].push_back('"');
			spec.attr[1].push_back('\'');
			break;

		case 6:
			// Attribute-focused quote matrix: both values contain both quote
			// characters, escapes and multibyte scalars.
			spec.attr[0].clear();
			spec.attr[0].push_back(0x1f642);
			spec.attr[0].push_back('"');
			spec.attr[0].push_back(0x4e2d);
			spec.attr[0].push_back('\'');
			spec.attr[0].push_back('&');
			spec.attr[0].push_back('<');
			spec.attr[0].push_back('>');
			spec.attr[0].push_back(0x10000);

			spec.attr[1].clear();
			spec.attr[1].push_back(0x10ffff);
			spec.attr[1].push_back('\'');
			spec.attr[1].push_back(0x4e2d);
			spec.attr[1].push_back('"');
			spec.attr[1].push_back('&');
			spec.attr[1].push_back('<');
			break;

		case 7:
			// Multiple legal CDATA end-sequence splits with multibyte bytes
			// immediately around them.
			spec.cdata.clear();
			spec.cdata.push_back(0x4e2d);
			spec.cdata.push_back(']');
			spec.cdata.push_back(']');
			spec.cdata.push_back('>');
			spec.cdata.push_back(0x1f642);
			spec.cdata.push_back(']');
			spec.cdata.push_back(']');
			spec.cdata.push_back('>');
			spec.cdata.push_back('X');
			spec.cdata.push_back(0x10000);
			spec.cdata.push_back(']');
			spec.cdata.push_back(']');
			spec.cdata.push_back('>');
			spec.cdata.push_back(0xfffd);
			spec.cdata.push_back(0x10ffff);
			break;

		case 8:
		{
			size_t long_length = native_buffer_capacity() * 3 + 37;
			if (long_length > 6000) long_length = 6000;

			spec.attr[0] = make_long_value(long_length / 2 + 17);
			spec.attr[1] = make_long_value(long_length / 3 + 11);
			spec.pcdata = make_long_value(long_length);
			spec.cdata = make_long_value(long_length / 2 + 5);
			break;
		}

		case 9:
		{
			size_t long_length = native_buffer_capacity() * 2 + 37;
			if (long_length > 6000) long_length = 6000;

			// Clean (no XML metacharacters) so format_no_escapes is a
			// meaningful alternate write path.  In both burst and chunked
			// mode this is short text, so it also covers the short-text
			// alternate write path.
			spec.attr[0] = make_long_value(3);
			spec.attr[1] = make_long_value(4);

			spec.pcdata.clear();
			spec.pcdata.push_back(0x00e9);
			spec.pcdata.push_back('T');
			spec.pcdata.push_back(0x4e2d);
			spec.pcdata.push_back(0x1f642);
			spec.pcdata.push_back('x');

			spec.cdata = make_long_value(5);
			break;
		}

		case 10:
		{
			// A clean long value exercising format_no_escapes past the output
			// buffer, including every UTF-8 length class.
			size_t long_length = native_buffer_capacity() * 2 + 37;
			if (long_length > 6000) long_length = 6000;

			spec.attr[0] = make_long_value(long_length / 2 + 17);
			spec.attr[1] = make_long_value(long_length / 3 + 11);
			spec.pcdata = make_long_value(long_length);
			spec.cdata = make_long_value(long_length / 2 + 5);
			break;
		}

		default:
			break;
		}

		spec.test_no_escapes = case_is_clean(spec);
		return spec;
	}

	// Fill a value with the full palette so every generated case covers all
	// UTF-8 byte-length classes and both BMP sides.  Additional filler is
	// deterministic from the supplied rng.
	inline std::vector<scalar_t> make_regular_value(Rng& rng, size_t target_length, bool allow_specials)
	{
		std::vector<scalar_t> value;

		// Guaranteed representation coverage.
		value.push_back('A');
		value.push_back(0x00e9);
		value.push_back(0x4e2d);
		value.push_back(0x1f642);

		size_t palette_count = 0;
		const scalar_t* palette = palette_scalars(palette_count);
		size_t special_count = 0;
		const scalar_t* specials = special_scalars(special_count);

		while (value.size() < target_length)
		{
			size_t pick = rng.below(12);

			if (allow_specials && pick == 0)
				value.push_back(specials[rng.below(special_count)]);
			else
				value.push_back(palette[rng.below(palette_count)]);
		}

		if (value.empty())
			value.push_back('A');

		return value;
	}

	inline CaseSpec make_random_case(Rng& rng)
	{
		CaseSpec spec;
		spec.name = "random";

		size_t capacity = native_buffer_capacity();
		unsigned int profile = static_cast<unsigned int>(rng.below(6));
		bool clean = (rng.below(4) == 0);

		bool allow_specials = !clean;

		size_t short_length = 6 + rng.below(8);
		size_t medium_length = capacity / 2 + rng.below(capacity / 4 + 1);
		size_t long_length = capacity * 2 + 37;
		if (long_length > 6000) long_length = 6000;

		spec.attr[0] = make_regular_value(rng, short_length, allow_specials);
		spec.attr[1] = make_regular_value(rng, short_length + rng.below(5), allow_specials);
		spec.pcdata = make_regular_value(rng, medium_length, allow_specials);
		spec.cdata = make_regular_value(rng, short_length, false);

		switch (profile)
		{
		case 0:
			// short values in every location
			break;

		case 1:
			// medium-length values
			spec.attr[0] = make_regular_value(rng, medium_length, allow_specials);
			spec.pcdata = make_regular_value(rng, medium_length, allow_specials);
			break;

		case 2:
			// values far beyond the output buffer in every location
			spec.attr[0] = make_regular_value(rng, long_length / 2 + 17, allow_specials);
			spec.attr[1] = make_regular_value(rng, long_length / 3 + 11, allow_specials);
			spec.pcdata = make_regular_value(rng, long_length, allow_specials);
			spec.cdata = make_regular_value(rng, long_length / 2 + 5, false);
			break;

		case 3:
			spec.pcdata = make_boundary_value();
			break;

		case 4:
			spec.attr[0] = make_boundary_value();
			break;

		case 5:
			spec.cdata = make_boundary_value();
			break;

		default:
			break;
		}

		// Occasionally include the legal CDATA continuation split.
		if (rng.below(3) == 0)
		{
			size_t at = spec.cdata.empty() ? 0 : rng.below(spec.cdata.size() + 1);

			spec.cdata.insert(spec.cdata.begin() + at, ']');
			spec.cdata.insert(spec.cdata.begin() + at + 1, ']');
			spec.cdata.insert(spec.cdata.begin() + at + 2, '>');
		}

		spec.test_no_escapes = case_is_clean(spec);
		return spec;
	}

	inline CaseSpec generate_case(const uint8_t* data, size_t size)
	{
		size_t family = 0;

		if (detect_fixed_family(data, size, family))
		{
			CaseSpec spec = make_fixed_case(family);
			spec.input_data = data;
			spec.input_size = size;
			return spec;
		}

		uint64_t hash = fnv1a_hash(data, size);
		Rng rng(hash);
		CaseSpec spec = make_random_case(rng);

		spec.name = "random.";
		spec.name += std::to_string(static_cast<unsigned long long>(hash & 0xffff));
		spec.input_data = data;
		spec.input_size = size;

		return spec;
	}

	// ---------- expected XML ------------------------------------------------

	inline void add_ascii(std::vector<scalar_t>& out, const char* text)
	{
		while (*text)
			out.push_back(static_cast<unsigned char>(*text++));
	}

	inline void add_char(std::vector<scalar_t>& out, scalar_t cp)
	{
		out.push_back(cp);
	}

	inline void append_escaped_pcdata(std::vector<scalar_t>& out, const std::vector<scalar_t>& value)
	{
		for (size_t i = 0; i < value.size(); ++i)
		{
			switch (value[i])
			{
			case '&': add_ascii(out, "&amp;"); break;
			case '<': add_ascii(out, "&lt;"); break;
			case '>': add_ascii(out, "&gt;"); break;
			default: add_char(out, value[i]); break;
			}
		}
	}

	inline void append_escaped_attr(std::vector<scalar_t>& out, const std::vector<scalar_t>& value, bool single_quote)
	{
		for (size_t i = 0; i < value.size(); ++i)
		{
			scalar_t cp = value[i];

			if (cp == '&') add_ascii(out, "&amp;");
			else if (cp == '<') add_ascii(out, "&lt;");
			else if (cp == '"') { if (single_quote) add_char(out, '"'); else add_ascii(out, "&quot;"); }
			else if (cp == '\'') { if (single_quote) add_ascii(out, "&apos;"); else add_char(out, '\''); }
			else add_char(out, cp); // '>' is literal inside attribute values
		}
	}

	inline void append_raw(std::vector<scalar_t>& out, const std::vector<scalar_t>& value)
	{
		out.insert(out.end(), value.begin(), value.end());
	}

	// pugixml emits the legal split of "]]>" inside CDATA as
	// "]]]]><![CDATA[>" (four closing brackets, then a new section opener).
	inline void append_cdata_value(std::vector<scalar_t>& out, const std::vector<scalar_t>& value)
	{
		add_ascii(out, "<![CDATA[");

		size_t i = 0;
		while (i < value.size())
		{
			if (i + 2 < value.size() && value[i] == ']' && value[i + 1] == ']' && value[i + 2] == '>')
			{
				add_char(out, ']');
				add_char(out, ']');
				add_char(out, ']');
				add_char(out, ']');
				add_char(out, '>');
				add_ascii(out, "<![CDATA[");
				add_char(out, '>');
				i += 3;
			}
			else
			{
				add_char(out, value[i]);
				++i;
			}
		}

		add_ascii(out, "]]>");
	}

	inline std::vector<scalar_t> build_expected(const CaseSpec& spec, bool single_quote, bool no_escapes)
	{
		std::vector<scalar_t> out;

		add_ascii(out, "<root");

		for (size_t a = 0; a < 2; ++a)
		{
			add_char(out, ' ');
			add_char(out, a == 0 ? 'a' : 'b');
			add_char(out, '=');
			add_char(out, single_quote ? '\'' : '"');

			if (no_escapes)
				append_raw(out, spec.attr[a]);
			else
				append_escaped_attr(out, spec.attr[a], single_quote);

			add_char(out, single_quote ? '\'' : '"');
		}

		add_char(out, '>');

		if (no_escapes)
			append_raw(out, spec.pcdata);
		else
			append_escaped_pcdata(out, spec.pcdata);

		append_cdata_value(out, spec.cdata);

		add_ascii(out, "</root>");
		return out;
	}

	// ---------- document and tree snapshot ----------------------------------

	inline void dump_native_string(const pugi::char_t* value, std::string& out)
	{
		if (!value)
		{
			out += "<null>";
			return;
		}

		for (const pugi::char_t* p = value; *p; ++p)
		{
			unsigned int unit;

			if (sizeof(pugi::char_t) == 1)
				unit = static_cast<unsigned char>(*p);
			else
				unit = static_cast<unsigned int>(*p);

			char buffer[16];
			snprintf(buffer, sizeof(buffer), "%x.", unit);
			out += buffer;
		}
	}

	inline void snapshot_node(pugi::xml_node node, std::string& out)
	{
		out += "(";
		out += std::to_string(static_cast<int>(node.type()));
		out += "|";
		dump_native_string(node.name(), out);
		out += "|";
		dump_native_string(node.value(), out);
		out += "|";

		for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute())
		{
			out += "a";
			dump_native_string(attr.name(), out);
			out += "=";
			dump_native_string(attr.value(), out);
			out += ";";
		}

		out += "[";

		for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
			snapshot_node(child, out);

		out += "])";
	}

	inline std::string snapshot_document(const pugi::xml_document& doc)
	{
		std::string out;

		for (pugi::xml_node child = doc.first_child(); child; child = child.next_sibling())
			snapshot_node(child, out);

		return out;
	}

	inline void set_native_value(pugi::xml_attribute attr, const std::vector<scalar_t>& scalars)
	{
		// Append a zero terminator so the size-delimited set_value path sees
		// a well formed value; set_value(ptr, size) copies exactly `size`
		// units regardless of sizeof(char_t).
		std::vector<pugi::char_t> native = to_native(scalars);
		native.push_back(static_cast<pugi::char_t>(0));

		attr.set_value(&native[0], native.size() - 1);
	}

	inline void set_native_value(pugi::xml_node node, const std::vector<scalar_t>& scalars)
	{
		std::vector<pugi::char_t> native = to_native(scalars);
		native.push_back(static_cast<pugi::char_t>(0));

		node.set_value(&native[0], native.size() - 1);
	}

	inline void build_document(const CaseSpec& spec, pugi::xml_document& doc)
	{
		pugi::xml_node root = doc.append_child(PUGIXML_TEXT("root"));

		for (size_t a = 0; a < 2; ++a)
		{
			pugi::xml_attribute attr = root.append_attribute(a == 0 ? PUGIXML_TEXT("a") : PUGIXML_TEXT("b"));
			set_native_value(attr, spec.attr[a]);
		}

		pugi::xml_node text = root.append_child(pugi::node_pcdata);
		set_native_value(text, spec.pcdata);

		pugi::xml_node cdata = root.append_child(pugi::node_cdata);
		set_native_value(cdata, spec.cdata);
	}

	// ---------- checking -----------------------------------------------------

	struct CollectingWriter: public pugi::xml_writer
	{
		std::string bytes;
		size_t chunks;

		CollectingWriter(): bytes(), chunks(0)
		{
		}

		void write(const void* data, size_t size) PUGIXML_OVERRIDE
		{
			if (size)
				bytes.append(static_cast<const char*>(data), size);
			++chunks;
		}
	};

	struct Failure
	{
		std::string assertion;
		std::string case_name;
		std::string input_hex;
		std::string config;
		std::string encoding;
		std::string flags;
		std::string detail;

		size_t first_diff_byte;
		size_t scalar_index;
		size_t expected_size;
		size_t actual_size;
		std::string expected_byte;
		std::string actual_byte;
		std::string scalar_value;
		std::string expected_window;
		std::string actual_window;

		Failure(): first_diff_byte(0), scalar_index(0), expected_size(0), actual_size(0)
		{
		}
	};

	inline std::string byte_description(const std::string& bytes, size_t index)
	{
		if (index >= bytes.size()) return "<past_end>";

		char buffer[16];
		snprintf(buffer, sizeof(buffer), "0x%02X", static_cast<unsigned int>(static_cast<unsigned char>(bytes[index])));
		return buffer;
	}

	inline std::string window_hex(const std::string& bytes, size_t center, size_t radius)
	{
		size_t begin = center > radius ? center - radius : 0;
		size_t end = std::min(bytes.size(), center + radius + 1);

		std::string out;

		for (size_t i = begin; i < end; ++i)
		{
			if (i != begin) out += " ";

			char buffer[8];
			snprintf(buffer, sizeof(buffer), "%02x", static_cast<unsigned int>(static_cast<unsigned char>(bytes[i])));
			out += buffer;
		}

		if (begin > 0) out = "..." + out;
		if (end < bytes.size()) out += "...";

		return out;
	}

	inline std::string scalar_description(scalar_t cp)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned int>(cp));
		return buffer;
	}

	inline void fill_mismatch(Failure& fail, const CaseSpec& spec, const OutputEncoding& encoding, bool single_quote, bool no_escapes, const std::vector<scalar_t>& expected_scalars, const std::vector<size_t>& byte_to_scalar, const std::string& expected_bytes, const std::string& actual_bytes)
	{
		fail.assertion = "content.";
		fail.assertion += spec.name;
		fail.assertion += ".";
		fail.assertion += encoding.name;
		fail.assertion += single_quote ? ".single_quote" : ".double_quote";
		fail.assertion += no_escapes ? ".no_escapes" : ".escaped";

		fail.case_name = spec.name;
		fail.input_hex = bytes_to_hex(spec.input_data, spec.input_size);
		fail.config = config_id();
		fail.encoding = encoding.name;

		fail.flags = "format_raw|format_no_declaration";
		if (single_quote) fail.flags += "|format_attribute_single_quote";
		if (no_escapes) fail.flags += "|format_no_escapes";

		size_t common = std::min(expected_bytes.size(), actual_bytes.size());
		size_t offset = 0;
		while (offset < common && expected_bytes[offset] == actual_bytes[offset]) ++offset;

		fail.first_diff_byte = offset;
		fail.expected_size = expected_bytes.size();
		fail.actual_size = actual_bytes.size();
		fail.expected_byte = byte_description(expected_bytes, offset);
		fail.actual_byte = byte_description(actual_bytes, offset);

		if (offset < byte_to_scalar.size())
			fail.scalar_index = byte_to_scalar[offset];
		else
			fail.scalar_index = expected_scalars.size();

		if (fail.scalar_index < expected_scalars.size())
			fail.scalar_value = scalar_description(expected_scalars[fail.scalar_index]);
		else
			fail.scalar_value = "<past_end>";

		fail.expected_window = window_hex(expected_bytes, offset, 8);
		fail.actual_window = window_hex(actual_bytes, offset, 8);
	}

	inline void fill_tree_failure(Failure& fail, const CaseSpec& spec, const std::string& before, const std::string& after)
	{
		fail.assertion = "content.";
		fail.assertion += spec.name;
		fail.assertion += ".tree_immutable";
		fail.case_name = spec.name;
		fail.input_hex = bytes_to_hex(spec.input_data, spec.input_size);
		fail.config = config_id();
		fail.encoding = "n/a";
		fail.flags = "format_raw|format_no_declaration";
		fail.detail = "document tree changed across save";
		fail.expected_size = before.size();
		fail.actual_size = after.size();
		fail.expected_window = before.substr(0, std::min<size_t>(before.size(), 512));
		fail.actual_window = after.substr(0, std::min<size_t>(after.size(), 512));
	}

	inline void print_failure(const Failure& fail, FILE* out)
	{
		fprintf(out, "CONTENT_ASSERTION_FAILED: %s\n", fail.assertion.c_str());
		fprintf(out, "  case=%s\n", fail.case_name.c_str());
		fprintf(out, "  input_hex=%s\n", fail.input_hex.c_str());
		fprintf(out, "  config=%s\n", fail.config.c_str());
		fprintf(out, "  encoding=%s\n", fail.encoding.c_str());
		fprintf(out, "  flags=%s\n", fail.flags.c_str());

		if (!fail.detail.empty())
			fprintf(out, "  detail=%s\n", fail.detail.c_str());

		fprintf(out, "  first_diff_byte=%zu\n", fail.first_diff_byte);
		fprintf(out, "  expected_byte=%s actual_byte=%s\n", fail.expected_byte.c_str(), fail.actual_byte.c_str());
		fprintf(out, "  scalar_index=%zu scalar=%s\n", fail.scalar_index, fail.scalar_value.c_str());
		fprintf(out, "  expected_len=%zu actual_len=%zu\n", fail.expected_size, fail.actual_size);
		fprintf(out, "  expected_window=%s\n", fail.expected_window.c_str());
		fprintf(out, "  actual_window=%s\n", fail.actual_window.c_str());
		fprintf(out, "  replay=serialize_model_main --input %s\n", fail.input_hex.c_str());
	}

	inline bool run_variant(const CaseSpec& spec, const pugi::xml_document& doc, const OutputEncoding& encoding, bool single_quote, bool no_escapes, Failure& fail)
	{
		std::vector<scalar_t> expected_scalars = build_expected(spec, single_quote, no_escapes);
		std::vector<size_t> byte_to_scalar;
		std::string expected_bytes = encode_scalars(expected_scalars, encoding.value, &byte_to_scalar);

		CollectingWriter writer;

		unsigned int flags = pugi::format_raw | pugi::format_no_declaration;
		if (single_quote) flags |= pugi::format_attribute_single_quote;
		if (no_escapes) flags |= pugi::format_no_escapes;

		doc.save(writer, PUGIXML_TEXT(""), flags, encoding.value);

		// Compare only the concatenation of all writer callbacks.  The number
		// and boundaries of the callbacks are deliberately ignored.
		if (writer.bytes == expected_bytes)
			return true;

		fill_mismatch(fail, spec, encoding, single_quote, no_escapes, expected_scalars, byte_to_scalar, expected_bytes, writer.bytes);
		return false;
	}

	inline bool run_case(const CaseSpec& spec, Failure& fail, size_t* executions_by_encoding)
	{
		pugi::xml_document doc;
		build_document(spec, doc);

		// The tree must be identical before and after the whole save matrix.
		std::string tree_before = snapshot_document(doc);

		size_t encoding_count = 0;
		const OutputEncoding* encodings = output_encodings(encoding_count);

		size_t local_counts[8] = { 0 };
		if (encoding_count > sizeof(local_counts) / sizeof(local_counts[0]))
			encoding_count = sizeof(local_counts) / sizeof(local_counts[0]);

		for (size_t e = 0; e < encoding_count; ++e)
		{
			// Escaped text, both attribute quote formats.
			++local_counts[e];
			if (!run_variant(spec, doc, encodings[e], false, false, fail)) goto failed;

			++local_counts[e];
			if (!run_variant(spec, doc, encodings[e], true, false, fail)) goto failed;

			// format_no_escapes only when the text has no XML metacharacters,
			// so it is a valid alternate write path rather than invalid XML.
			if (spec.test_no_escapes)
			{
				++local_counts[e];
				if (!run_variant(spec, doc, encodings[e], false, true, fail)) goto failed;

				++local_counts[e];
				if (!run_variant(spec, doc, encodings[e], true, true, fail)) goto failed;
			}
		}

		{
			std::string tree_after = snapshot_document(doc);

			if (tree_before != tree_after)
			{
				fill_tree_failure(fail, spec, tree_before, tree_after);
				goto failed;
			}
		}

		if (executions_by_encoding)
		{
			for (size_t e = 0; e < encoding_count; ++e)
				executions_by_encoding[e] += local_counts[e];
		}

		return true;

	failed:
		if (executions_by_encoding)
		{
			for (size_t e = 0; e < encoding_count; ++e)
				executions_by_encoding[e] += local_counts[e];
		}

		return false;
	}

	inline bool check_input(const uint8_t* data, size_t size, Failure& fail, size_t* executions_by_encoding = 0)
	{
		CaseSpec spec = generate_case(data, size);
		return run_case(spec, fail, executions_by_encoding);
	}

	// Model self-check: every generated document and every fixed family must
	// really exercise the content paths this suite claims to cover.  It is
	// deliberately independent from the byte comparison so that a silently
	// skipped or empty case cannot masquerade as a passing one.
	struct InvariantReport
	{
		bool ok;
		std::string detail;

		InvariantReport(): ok(true)
		{
		}
	};

	inline void invariant_require(InvariantReport& report, bool condition, const std::string& message)
	{
		if (!condition)
		{
			report.ok = false;
			if (!report.detail.empty()) report.detail += "; ";
			report.detail += message;
		}
	}

	inline bool any_non_ascii(const std::vector<scalar_t>& value)
	{
		for (size_t i = 0; i < value.size(); ++i)
			if (value[i] >= 0x80) return true;

		return false;
	}

	inline bool any_supplementary(const std::vector<scalar_t>& value)
	{
		for (size_t i = 0; i < value.size(); ++i)
			if (value[i] >= 0x10000) return true;

		return false;
	}

	inline bool any_utf8_length_class(const std::vector<scalar_t>& value, size_t byte_length)
	{
		for (size_t i = 0; i < value.size(); ++i)
		{
			scalar_t cp = value[i];
			size_t actual = cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4));

			if (actual == byte_length) return true;
		}

		return false;
	}

	inline bool carriers_have_utf8_length_class(const CaseSpec& spec, size_t byte_length)
	{
		return any_utf8_length_class(spec.attr[0], byte_length) ||
			any_utf8_length_class(spec.attr[1], byte_length) ||
			any_utf8_length_class(spec.pcdata, byte_length) ||
			any_utf8_length_class(spec.cdata, byte_length);
	}

	inline bool any_escape_char(const std::vector<scalar_t>& value)
	{
		for (size_t i = 0; i < value.size(); ++i)
			if (scalar_is_xml_special(value[i]) || value[i] < 0x20) return true;

		return false;
	}

	inline bool any_cdata_end_split(const std::vector<scalar_t>& value)
	{
		for (size_t i = 0; i + 2 < value.size(); ++i)
			if (value[i] == ']' && value[i + 1] == ']' && value[i + 2] == '>') return true;

		return false;
	}

	inline bool carrier_exceeds_buffer(const std::vector<scalar_t>& value)
	{
		return to_native(value).size() > native_buffer_capacity();
	}

	// Verify a generated case carries non-ASCII and supplementary content in
	// at least one carrier.  This is independent of the byte comparison, so
	// an empty or ASCII-only case cannot pass silently.  For random cases the
	// full four-class UTF-8 matrix is additionally required; purpose-built
	// fixed families are checked for the suite-wide matrix instead.
	inline void check_case_invariants(const CaseSpec& spec, InvariantReport& report, bool require_full_matrix = false)
	{
		bool has_non_ascii = any_non_ascii(spec.attr[0]) || any_non_ascii(spec.attr[1]) || any_non_ascii(spec.pcdata) || any_non_ascii(spec.cdata);

		invariant_require(report, has_non_ascii, "case " + spec.name + " has no non-ASCII content");

		bool has_supplementary = any_supplementary(spec.attr[0]) || any_supplementary(spec.attr[1]) || any_supplementary(spec.pcdata) || any_supplementary(spec.cdata);

		invariant_require(report, has_supplementary, "case " + spec.name + " has no supplementary-plane content");

		if (!require_full_matrix) return;

		for (size_t n = 1; n <= 4; ++n)
		{
			if (!carriers_have_utf8_length_class(spec, n))
			{
				report.ok = false;
				if (!report.detail.empty()) report.detail += "; ";
				report.detail += "case " + spec.name + " lacks a " + std::to_string(n) + "-byte UTF-8 scalar";
			}
		}
	}

	// Verify the aggregate coverage the fixed families must provide: buffer
	// crossing, escapes, both attribute quote characters, a legal CDATA end
	// split, a clean no_escapes case, and long values in every carrier.
	inline void check_fixed_suite_invariants(InvariantReport& report)
	{
		bool crossing = false;
		bool escapes = false;
		bool double_quote = false;
		bool single_quote = false;
		bool cdata_split = false;
		bool no_escapes_case = false;
		bool long_attr = false;
		bool long_pcdata = false;
		bool long_cdata = false;

		size_t boundary_families = 0;

		for (size_t f = 0; f < fixed_family_count(); ++f)
		{
			CaseSpec spec = make_fixed_case(f);

			if (carrier_exceeds_buffer(spec.attr[0]) || carrier_exceeds_buffer(spec.attr[1]))
				long_attr = true;
			if (carrier_exceeds_buffer(spec.pcdata))
				long_pcdata = true;
			if (carrier_exceeds_buffer(spec.cdata))
				long_cdata = true;

			if (long_attr || long_pcdata || long_cdata)
				crossing = true;

			if (any_escape_char(spec.attr[0]) || any_escape_char(spec.attr[1]) || any_escape_char(spec.pcdata))
				escapes = true;

			for (size_t i = 0; i < spec.attr[0].size(); ++i)
			{
				if (spec.attr[0][i] == '"') double_quote = true;
				if (spec.attr[0][i] == '\'') single_quote = true;
			}

			for (size_t i = 0; i < spec.attr[1].size(); ++i)
			{
				if (spec.attr[1][i] == '"') double_quote = true;
				if (spec.attr[1][i] == '\'') single_quote = true;
			}

			if (any_cdata_end_split(spec.cdata))
				cdata_split = true;

			if (spec.test_no_escapes)
				no_escapes_case = true;

			if (spec.name.find("align") != std::string::npos && (carrier_exceeds_buffer(spec.pcdata) || carrier_exceeds_buffer(spec.attr[0]) || carrier_exceeds_buffer(spec.cdata)))
				++boundary_families;
		}

		invariant_require(report, crossing, "fixed suite never crosses the native output buffer");
		invariant_require(report, boundary_families >= 3, "fixed suite lacks pcdata/attribute/CDATA boundary families");
		invariant_require(report, escapes, "fixed suite lacks escapable content");
		invariant_require(report, double_quote, "fixed suite lacks a double quote in attributes");
		invariant_require(report, single_quote, "fixed suite lacks a single quote in attributes");
		invariant_require(report, cdata_split, "fixed suite lacks a legal CDATA end-sequence split");
		invariant_require(report, no_escapes_case, "fixed suite lacks a clean format_no_escapes case");
		invariant_require(report, long_attr, "fixed suite lacks long attribute values");
		invariant_require(report, long_pcdata, "fixed suite lacks long text values");
		invariant_require(report, long_cdata, "fixed suite lacks long CDATA values");

		// Across the suite, every UTF-8 length class must be present.
		bool class_present[5] = { false, false, false, false, false };

		for (size_t f = 0; f < fixed_family_count(); ++f)
		{
			CaseSpec spec = make_fixed_case(f);

			for (size_t n = 1; n <= 4; ++n)
				if (carriers_have_utf8_length_class(spec, n)) class_present[n] = true;
		}

		for (size_t n = 1; n <= 4; ++n)
		{
			if (!class_present[n])
			{
				report.ok = false;
				if (!report.detail.empty()) report.detail += "; ";
				report.detail += "fixed suite lacks a " + std::to_string(n) + "-byte UTF-8 scalar";
			}
		}
	}


} // namespace serialize_model

#endif
