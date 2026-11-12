/**
 * XInclude assembly helper for pugixml - implementation
 * --------------------------------------------------------
 * SPDX-License-Identifier: MIT
 *
 * See extras/xinclude.hpp for the public contract. The implementation is a
 * self contained assembler: it performs no file system or network access,
 * all resources come from the resolver callback, and the input document is
 * never modified.
 */

#include "xinclude.hpp"

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pugi
{
	namespace xinclude
	{
		const char* const XINCLUDE_NAMESPACE = "http://www.w3.org/2001/XInclude";
		const char* const XML_NAMESPACE = "http://www.w3.org/XML/1998/namespace";

		namespace
		{
			// ---------------------------------------------------------------------
			// URI parsing and reference resolution (RFC 3986 subset)
			// ---------------------------------------------------------------------

			struct Uri
			{
				std::string scheme;
				std::string authority;     // empty when has_authority is false
				bool has_authority;
				std::string path;
				std::string query;
				std::string fragment;

				Uri(): has_authority(false)
				{
				}
			};

			bool is_scheme_first(char c)
			{
				return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
			}

			bool is_scheme_char(char c)
			{
				return is_scheme_first(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
			}

			// Parses either an absolute URI or a relative reference.
			bool parse_uri(const std::string& text, Uri& out)
			{
				out = Uri();

				const std::size_t n = text.size();
				std::size_t pos = 0;

				// scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
				if (n > 0 && is_scheme_first(text[0]))
				{
					std::size_t colon = std::string::npos;
					for (std::size_t i = 0; i < n; ++i)
					{
						char c = text[i];
						if (c == ':') { colon = i; break; }
						if (c == '/' || c == '?' || c == '#' || !is_scheme_char(c)) break;
					}

					if (colon != std::string::npos)
					{
						out.scheme = text.substr(0, colon);
						pos = colon + 1;
					}
				}

				std::string remainder = text.substr(pos);

				std::size_t hash = remainder.find('#');
				std::size_t question = remainder.find('?');
				if (hash != std::string::npos && question != std::string::npos && question > hash)
					question = std::string::npos; // '?' belongs to the fragment

				if (hash != std::string::npos)
				{
					out.fragment = remainder.substr(hash + 1);
					remainder = remainder.substr(0, hash);
				}

				if (question != std::string::npos)
				{
					out.query = remainder.substr(question + 1);
					remainder = remainder.substr(0, question);
				}

				if (remainder.compare(0, 2, "//") == 0)
				{
					out.has_authority = true;
					std::size_t slash = remainder.find('/', 2);
					if (slash == std::string::npos)
					{
						out.authority = remainder.substr(2);
						out.path = "";
					}
					else
					{
						out.authority = remainder.substr(2, slash - 2);
						out.path = remainder.substr(slash);
					}
				}
				else
				{
					out.path = remainder;
				}

				return true;
			}

			bool is_absolute(const Uri& u)
			{
				return !u.scheme.empty() || (!u.path.empty() && u.path[0] == '/');
			}

			// Removes "." and ".." segments (RFC 3986 section 5.2.4).
			std::string normalize_path(const std::string& path)
			{
				bool leading = !path.empty() && path[0] == '/';
				bool trailing = path.size() > 1 && path[path.size() - 1] == '/';

				std::vector<std::string> stack;
				std::size_t start = 0;

				while (start <= path.size())
				{
					std::size_t slash = path.find('/', start);
					std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);

					if (segment == "..")
					{
						if (!stack.empty() && stack.back() != "..")
							stack.pop_back();
						else if (!leading)
							stack.push_back("..");
					}
					else if (!segment.empty() && segment != ".")
					{
						stack.push_back(segment);
					}

					if (slash == std::string::npos) break;
					start = slash + 1;
				}

				std::string result;
				if (leading) result += '/';
				for (std::size_t i = 0; i < stack.size(); ++i)
				{
					if (i) result += '/';
					result += stack[i];
				}
				if (trailing)
				{
					if (result.empty()) result = leading ? "/" : std::string();
					else if (result[result.size() - 1] != '/') result += '/';
				}

				return result;
			}

			// Resolves a reference against a base URI (RFC 3986 section 5.2).
			Uri resolve_reference(const Uri& base, const Uri& ref)
			{
				Uri target;

				if (!ref.scheme.empty())
				{
					target.scheme = ref.scheme;
					target.has_authority = ref.has_authority;
					target.authority = ref.authority;
					target.path = normalize_path(ref.path);
					target.query = ref.query;
				}
				else
				{
					target.scheme = base.scheme;

					if (ref.has_authority)
					{
						target.has_authority = true;
						target.authority = ref.authority;
						target.path = normalize_path(ref.path);
						target.query = ref.query;
					}
					else
					{
						target.has_authority = base.has_authority;
						target.authority = base.authority;

						if (ref.path.empty())
						{
							target.path = base.path;
							target.query = ref.query.empty() ? base.query : ref.query;
						}
						else
						{
							if (!ref.path.empty() && ref.path[0] == '/')
							{
								target.path = normalize_path(ref.path);
							}
							else
							{
								std::string merged;
								if (base.has_authority && base.path.empty())
									merged = "/" + ref.path;
								else
								{
									std::size_t slash = base.path.rfind('/');
									merged = (slash == std::string::npos ? "" : base.path.substr(0, slash + 1)) + ref.path;
								}
								target.path = normalize_path(merged);
							}
							target.query = ref.query;
						}
					}
				}

				target.fragment = ref.fragment;
				return target;
			}

			std::string recompose(const Uri& u)
			{
				std::string result;
				if (!u.scheme.empty()) result += u.scheme + ":";
				if (u.has_authority) result += "//" + u.authority;
				result += u.path;
				if (!u.query.empty()) result += "?" + u.query;
				if (!u.fragment.empty()) result += "#" + u.fragment;
				return result;
			}

			// ---------------------------------------------------------------------
			// Namespace and xml: attribute helpers
			// ---------------------------------------------------------------------

			std::string name_prefix(const char* name)
			{
				const char* colon = std::strchr(name, ':');
				return colon ? std::string(name, static_cast<std::size_t>(colon - name)) : std::string();
			}

			std::string name_local(const char* name)
			{
				const char* colon = std::strchr(name, ':');
				return colon ? std::string(colon + 1) : std::string(name);
			}

			// Returns the namespace URI bound to prefix at node ("" = default
			// namespace), walking xmlns declarations towards the document root.
			std::string lookup_namespace(xml_node node, const std::string& prefix)
			{
				for (xml_node cur = node; cur; cur = cur.parent())
				{
					if (cur.type() != node_element) continue;

					std::string attr_name = prefix.empty() ? "xmlns" : "xmlns:" + prefix;
					xml_attribute attr = cur.attribute(attr_name.c_str());
					if (attr) return attr.value();
				}

				if (prefix == "xml") return XML_NAMESPACE;
				return std::string();
			}

			bool is_xinclude_element(xml_node node, const char* local)
			{
				if (node.type() != node_element) return false;
				if (name_local(node.name()) != local) return false;
				return lookup_namespace(node, name_prefix(node.name())) == XINCLUDE_NAMESPACE;
			}

			// ---------------------------------------------------------------------
			// Text resource decoding (UTF-8 and UTF-16LE/BE only)
			// ---------------------------------------------------------------------

			std::string ascii_lower(std::string s)
			{
				for (char& c : s)
					if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
				return s;
			}

			// Extracts the encoding name from an XML declaration in an ASCII
			// preview of the resource. Plain text that merely mentions
			// "encoding=" is not treated as a declaration.
			std::string scan_decl_encoding(const std::string& preview)
			{
				if (preview.compare(0, 5, "<?xml") != 0) return std::string();

				const std::string key = "encoding";
				std::size_t p = preview.find(key);
				if (p == std::string::npos) return std::string();

				p += key.size();
				while (p < preview.size() && (preview[p] == ' ' || preview[p] == '\t' || preview[p] == '\n' || preview[p] == '\r')) ++p;
				if (p >= preview.size() || preview[p] != '=') return std::string();
				++p;
				while (p < preview.size() && (preview[p] == ' ' || preview[p] == '\t' || preview[p] == '\n' || preview[p] == '\r')) ++p;
				if (p >= preview.size() || (preview[p] != '"' && preview[p] != '\'')) return std::string();

				char quote = preview[p++];
				std::size_t begin = p;
				while (p < preview.size() && preview[p] != quote) ++p;
				if (p >= preview.size()) return std::string();

				return ascii_lower(preview.substr(begin, p - begin));
			}

			void append_utf8(std::string& out, std::uint32_t cp)
			{
				if (cp < 0x80)
				{
					out += static_cast<char>(cp);
				}
				else if (cp < 0x800)
				{
					out += static_cast<char>(0xC0 | (cp >> 6));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				}
				else if (cp < 0x10000)
				{
					out += static_cast<char>(0xE0 | (cp >> 12));
					out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				}
				else
				{
					out += static_cast<char>(0xF0 | (cp >> 18));
					out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
					out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (cp & 0x3F));
				}
			}

			bool decode_utf8(const std::uint8_t* data, std::size_t size, std::string& out)
			{
				out.clear();
				out.reserve(size);

				for (std::size_t i = 0; i < size;)
				{
					std::uint8_t b0 = data[i];
					std::uint32_t cp;
					std::size_t length;

					if (b0 < 0x80) { cp = b0; length = 1; }
					else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; length = 2; }
					else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; length = 3; }
					else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; length = 4; }
					else return false;

					if (i + length > size) return false;

					for (std::size_t k = 1; k < length; ++k)
					{
						std::uint8_t bk = data[i + k];
						if ((bk & 0xC0) != 0x80) return false;
						cp = (cp << 6) | (bk & 0x3F);
					}

					// Reject overlong sequences, surrogates and out of range values.
					if (length == 2 && cp < 0x80) return false;
					if (length == 3 && cp < 0x800) return false;
					if (length == 4 && cp < 0x10000) return false;
					if (cp >= 0xD800 && cp <= 0xDFFF) return false;
					if (cp > 0x10FFFF) return false;

					append_utf8(out, cp);
					i += length;
				}

				return true;
			}

			bool decode_utf16(const std::uint8_t* data, std::size_t size, bool big_endian, std::string& out)
			{
				out.clear();
				if (size % 2 != 0) return false;
				out.reserve(size / 2);

				for (std::size_t i = 0; i + 1 < size; i += 2)
				{
					std::uint16_t unit = big_endian
						? static_cast<std::uint16_t>((data[i] << 8) | data[i + 1])
						: static_cast<std::uint16_t>(data[i] | (data[i + 1] << 8));

					std::uint32_t cp;
					if (unit >= 0xD800 && unit <= 0xDBFF)
					{
						if (i + 3 >= size) return false;
						std::uint16_t low = big_endian
							? static_cast<std::uint16_t>((data[i + 2] << 8) | data[i + 3])
							: static_cast<std::uint16_t>(data[i + 2] | (data[i + 3] << 8));
						if (low < 0xDC00 || low > 0xDFFF) return false;
						cp = 0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) + (low - 0xDC00);
						i += 2;
					}
					else if (unit >= 0xDC00 && unit <= 0xDFFF)
					{
						return false; // unmatched low surrogate
					}
					else
					{
						cp = unit;
					}

					append_utf8(out, cp);
				}

				return true;
			}

			void normalize_eol(std::string& text)
			{
				std::string result;
				result.reserve(text.size());
				for (std::size_t i = 0; i < text.size(); ++i)
				{
					if (text[i] != '\r') { result += text[i]; continue; }
					result += '\n';
					if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
				}
				text.swap(result);
			}

			enum TextEncoding { text_enc_utf8, text_enc_utf16_le, text_enc_utf16_be, text_enc_unknown };

			// Decodes a text resource. Returns false on any encoding conflict or
			// malformed byte sequence.
			bool decode_text_resource(const std::uint8_t* d, std::size_t size, std::string& out)
			{
				TextEncoding bom = text_enc_unknown;
				std::size_t bom_length = 0;

				if (size >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF)
				{
					bom = text_enc_utf8;
					bom_length = 3;
				}
				else if (size >= 2 && d[0] == 0xFF && d[1] == 0xFE)
				{
					bom = text_enc_utf16_le;
					bom_length = 2;
				}
				else if (size >= 2 && d[0] == 0xFE && d[1] == 0xFF)
				{
					bom = text_enc_utf16_be;
					bom_length = 2;
				}

				// Build an ASCII preview of the declaration area so the encoding
				// pseudo-attribute can be read for both UTF-8 and UTF-16 bytes.
				bool preview_utf16 = (bom == text_enc_utf16_le || bom == text_enc_utf16_be);
				bool preview_be = (bom == text_enc_utf16_be);

				if (bom == text_enc_unknown && size >= 2)
				{
					// XML requires UTF-16 entities without a BOM to begin with an
					// encoding declaration; the leading "<" reveals the width.
					if (d[0] == 0x00 && d[1] == 0x3C) { preview_utf16 = true; preview_be = true; }
					else if (d[0] == 0x3C && d[1] == 0x00) { preview_utf16 = true; preview_be = false; }
				}

				std::string preview;
				if (preview_utf16)
				{
					for (std::size_t i = bom_length; i + 1 < size && preview.size() < 256; i += 2)
					{
						std::uint16_t unit = preview_be ? static_cast<std::uint16_t>((d[i] << 8) | d[i + 1])
						                                : static_cast<std::uint16_t>(d[i] | (d[i + 1] << 8));
						preview += (unit < 0x80) ? static_cast<char>(unit) : ' ';
					}
				}
				else
				{
					std::size_t preview_size = size - bom_length;
					preview.assign(reinterpret_cast<const char*>(d + bom_length), preview_size < 256 ? preview_size : 256);
				}

				std::string declared = scan_decl_encoding(preview);
				TextEncoding effective;

				if (bom != text_enc_unknown)
				{
					effective = bom;
					if (!declared.empty())
					{
						TextEncoding declared_enc = text_enc_unknown;
						if (declared == "utf-8") declared_enc = text_enc_utf8;
						else if (declared == "utf-16le") declared_enc = text_enc_utf16_le;
						else if (declared == "utf-16be") declared_enc = text_enc_utf16_be;
						else if (declared == "utf-16") declared_enc = text_enc_unknown; // the BOM disambiguates
						else return false;
						if (declared_enc != text_enc_unknown && declared_enc != bom)
							return false; // BOM/declaration conflict
					}
				}
				else if (declared == "utf-16le")
				{
					if (preview_utf16 && preview_be) return false; // declaration/bytes conflict
					effective = text_enc_utf16_le;
				}
				else if (declared == "utf-16be")
				{
					if (preview_utf16 && !preview_be) return false; // declaration/bytes conflict
					effective = text_enc_utf16_be;
				}
				else if (declared == "utf-16")
				{
					if (!preview_utf16) return false; // no way to pick an endianness
					effective = preview_be ? text_enc_utf16_be : text_enc_utf16_le;
				}
				else // "utf-8" or no declaration: trust the leading-byte width sniff
				{
					if (preview_utf16)
					{
						if (!declared.empty()) return false; // UTF-8 declaration on UTF-16 bytes
						effective = preview_be ? text_enc_utf16_be : text_enc_utf16_le;
					}
					else
					{
						effective = text_enc_utf8;
					}
				}

				const std::uint8_t* payload = d + bom_length;
				std::size_t payload_size = size - bom_length;
				bool ok = false;

				switch (effective)
				{
				case text_enc_utf8:
					ok = decode_utf8(payload, payload_size, out);
					break;
				case text_enc_utf16_le:
					ok = decode_utf16(payload, payload_size, false, out);
					break;
				case text_enc_utf16_be:
					ok = decode_utf16(payload, payload_size, true, out);
					break;
				default:
					return false;
				}

				if (!ok) return false;
				normalize_eol(out);
				return true;
			}

			// ---------------------------------------------------------------------
			// Assembler
			// ---------------------------------------------------------------------

			class Assembler
			{
			public:
				explicit Assembler(const ResourceResolver& resolver): resolver_(resolver)
				{
				}

				Result run(const xml_document& input, const std::string& document_uri)
				{
					Result result;

					Uri base;
					if (!parse_uri(document_uri, base) || !is_absolute(base) || !base.query.empty() || !base.fragment.empty())
					{
						fail(xi_error_invalid_uri, "main document URI is not an absolute URI without query/fragment", document_uri);
						result.error = error_;
						return result;
					}
					base.path = normalize_path(base.path);

					std::string base_text = recompose(base);
					chain_.push_back(base_text);

					// Independent copy: the caller's document is never touched.
					result.document.reset(input);

					if (process_tree(result.document, base))
					{
						chain_.pop_back();
						return result;
					}

					result.error = error_;
					result.document.reset();
					return result;
				}

			private:
				const ResourceResolver& resolver_;
				std::vector<std::string> chain_;
				Error error_;

				void fail(ErrorCode code, const std::string& message, const std::string& resource_uri)
				{
					error_.code = code;
					error_.message = message;
					error_.resource_uri = resource_uri;
					error_.include_chain = chain_;
					if (error_.include_chain.empty() || error_.include_chain.back() != resource_uri)
						error_.include_chain.push_back(resource_uri);
				}

				static bool fallbackable(ErrorCode code)
				{
					return code == xi_error_resource_not_found
						|| code == xi_error_xml_syntax
						|| code == xi_error_empty_selection
						|| code == xi_error_text_decoding;
				}

				// xml:base effective at node, resolved starting from root_base.
				Uri absolute_base_of(xml_node node, const Uri& root_base)
				{
					std::vector<xml_node> path;
					for (xml_node cur = node; cur && cur.type() != node_document; cur = cur.parent())
						path.push_back(cur);

					Uri base = root_base;
					for (std::size_t i = path.size(); i-- > 0;)
					{
						xml_attribute attr = path[i].attribute("xml:base");
						if (!attr) continue;
						Uri ref;
						if (!parse_uri(attr.value(), ref) || !ref.query.empty() || !ref.fragment.empty())
							continue; // validated earlier; defensive
						base = resolve_reference(base, ref);
					}
					return base;
				}

				static std::string nearest_lang(xml_node node)
				{
					for (xml_node cur = node; cur; cur = cur.parent())
					{
						xml_attribute attr = cur.attribute("xml:lang");
						if (attr) return attr.value();
					}
					return std::string();
				}

				// Effective base of a directive node, applying its own xml:base.
				bool base_for_node(xml_node node, const Uri& base, Uri& out)
				{
					out = base;
					if (node.type() != node_element) return true;
					xml_attribute base_attr = node.attribute("xml:base");
					if (!base_attr) return true;

					Uri ref;
					if (!parse_uri(base_attr.value(), ref) || !ref.query.empty() || !ref.fragment.empty())
					{
						fail(xi_error_unsupported_syntax, "xml:base uses a query or fragment URI", base_attr.value());
						return false;
					}
					out = resolve_reference(base, ref);
					return true;
				}

				// Expands every include directive in the subtree rooted at node.
				bool process_tree(xml_node node, const Uri& base)
				{
					xml_node child = node.first_child();
					while (child)
					{
						xml_node next = child.next_sibling();

						Uri child_base;
						if (!base_for_node(child, base, child_base)) return false;

						if (is_xinclude_element(child, "include"))
						{
							if (!handle_include(child, child_base)) return false;
						}
						else if (child.type() == node_element || child.type() == node_document)
						{
							if (!process_tree(child, child_base)) return false;
						}

						child = next;
					}
					return true;
				}

				struct IncludeSpec
				{
					std::string parse_mode;       // "xml" or "text"
					std::string href;
					bool has_href;
					std::string xpointer;
					bool has_xpointer;
					xml_node fallback;            // empty when absent
				};

				// Validates directive structure and reads its attributes.
				bool validate_include(xml_node include, IncludeSpec& spec)
				{
					spec.parse_mode = "xml";
					spec.has_href = false;
					spec.has_xpointer = false;

					xml_attribute parse_attr = include.attribute("parse");
					if (parse_attr)
					{
						spec.parse_mode = parse_attr.value();
						if (spec.parse_mode != "xml" && spec.parse_mode != "text")
						{
							fail(xi_error_structural, "unknown xi:include parse value: " + spec.parse_mode, chain_.back());
							return false;
						}
					}

					xml_attribute href_attr = include.attribute("href");
					if (href_attr)
					{
						spec.has_href = true;
						spec.href = href_attr.value();
					}

					xml_attribute xp_attr = include.attribute("xpointer");
					if (xp_attr)
					{
						spec.has_xpointer = true;
						spec.xpointer = xp_attr.value();
					}

					int fallback_count = 0;
					for (xml_node c = include.first_child(); c; c = c.next_sibling())
					{
						if (c.type() != node_element) continue;
						if (!is_xinclude_element(c, "fallback"))
						{
							fail(xi_error_structural, std::string("illegal child element of xi:include: ") + c.name(), chain_.back());
							return false;
						}
						++fallback_count;
						spec.fallback = c;
					}

					if (fallback_count > 1)
					{
						fail(xi_error_structural, "xi:include has more than one xi:fallback", chain_.back());
						return false;
					}

					return true;
				}

				bool parse_element_xpointer(const std::string& xpointer, std::vector<int>& steps)
				{
					const std::string prefix = "element(";
					if (xpointer.compare(0, prefix.size(), prefix) != 0 || xpointer.empty() || xpointer[xpointer.size() - 1] != ')')
						return false;

					std::string body = xpointer.substr(prefix.size(), xpointer.size() - prefix.size() - 1);
					if (body.empty() || body[0] != '/') return false;

					std::size_t pos = 1;
					while (pos < body.size())
					{
						std::size_t slash = body.find('/', pos);
						std::string token = body.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
						if (token.empty()) return false;
						int value = 0;
						for (char c : token)
						{
							if (c < '0' || c > '9') return false;
							if (value > (INT_MAX - 9) / 10) return false; // overflow guard
							value = value * 10 + (c - '0');
						}
						if (value < 1) return false;
						steps.push_back(value);
						if (slash == std::string::npos) break;
						pos = slash + 1;
					}
					return !steps.empty();
				}

				bool select_element(xml_document& doc, const std::vector<int>& steps, xml_node& selected)
				{
					xml_node current = doc;
					for (std::size_t s = 0; s < steps.size(); ++s)
					{
						int index = steps[s];
						int seen = 0;
						xml_node found;
						for (xml_node c = current.first_child(); c; c = c.next_sibling())
						{
							if (c.type() != node_element) continue;
							++seen;
							if (seen == index) { found = c; break; }
						}
						if (!found) return false;
						current = found;
					}
					selected = current;
					return true;
				}

				// Adds namespace declarations inherited from ancestors that the
				// selected root would lose once detached.
				void preserve_namespaces(xml_node original, xml_node copy)
				{
					// Collect nearest declaration per prefix (outer first, inner wins).
					std::vector<std::pair<std::string, std::string> > declarations;
					for (xml_node cur = original.parent(); cur; cur = cur.parent())
					{
						if (cur.type() != node_element) continue;
						for (xml_attribute a = cur.first_attribute(); a; a = a.next_attribute())
						{
							std::string attr = a.name();
							std::string prefix;
							if (attr == "xmlns") prefix = "";
							else if (attr.compare(0, 6, "xmlns:") == 0) prefix = attr.substr(6);
							else continue;

							bool known = false;
							for (std::size_t i = 0; i < declarations.size(); ++i)
								if (declarations[i].first == prefix) { known = true; break; }
							if (!known) declarations.push_back(std::make_pair(prefix, a.value()));
						}
					}

					for (std::size_t i = 0; i < declarations.size(); ++i)
					{
						const std::string& prefix = declarations[i].first;
						std::string attr_name = prefix.empty() ? "xmlns" : "xmlns:" + prefix;
						if (!copy.attribute(attr_name.c_str()))
							copy.append_attribute(attr_name.c_str()).set_value(declarations[i].second.c_str());
					}
				}

				// Preserves xml:lang and makes the absolute xml:base explicit when
				// the insertion position would change their effective values.
				void preserve_context(xml_node original, xml_node copy, xml_node include,
				                      const Uri& resource_base, const Uri& include_base)
				{
					if (copy.type() != node_element) return;

					std::string original_lang = nearest_lang(original);
					std::string position_lang = nearest_lang(include.parent());
					if (!original_lang.empty() && original_lang != position_lang)
						copy.attribute("xml:lang") ? copy.attribute("xml:lang").set_value(original_lang.c_str())
						                          : copy.append_attribute("xml:lang").set_value(original_lang.c_str());

					Uri original_base = absolute_base_of(original, resource_base);
					if (recompose(original_base) != recompose(include_base))
					{
						std::string base_text = recompose(original_base);
						copy.attribute("xml:base") ? copy.attribute("xml:base").set_value(base_text.c_str())
						                          : copy.append_attribute("xml:base").set_value(base_text.c_str());
					}
				}

				// Inserts copies of a processed resource node in place of include.
				bool insert_resource_nodes(xml_node source, xml_node include, const Uri& resource_base, const Uri& include_base)
				{
					xml_node parent = include.parent();

					if (source.type() == node_document)
					{
						for (xml_node c = source.first_child(); c; c = c.next_sibling())
						{
							// The XML declaration is not an infoset item; a doctype can
							// only live directly under a document node.
							if (c.type() == node_declaration) continue;
							if (c.type() == node_doctype && parent.type() != node_document) continue;

							xml_node copied = parent.insert_copy_before(c, include);
							if (!copied) return false;
							if (copied.type() == node_element)
								preserve_context(c, copied, include, resource_base, include_base);
						}
					}
					else
					{
						xml_node copied = include.parent().insert_copy_before(source, include);
						if (!copied) return false;
						preserve_namespaces(source, copied);
						preserve_context(source, copied, include, resource_base, include_base);
					}
					return include.parent().remove_child(include);
				}

				bool apply_fallback(xml_node fallback, xml_node include, const Uri& base)
				{
					for (xml_node c = fallback.first_child(); c; c = c.next_sibling())
					{
						xml_node copied = include.parent().insert_copy_before(c, include);
						if (!copied) return false;

						Uri copied_base;
						if (!base_for_node(copied, base, copied_base)) return false;

						// A fallback can itself contain include directives; the
						// copied node may be one, so check it before descending.
						if (is_xinclude_element(copied, "include"))
						{
							if (!handle_include(copied, copied_base)) return false;
						}
						else if (!process_tree(copied, copied_base))
						{
							return false;
						}
					}
					return include.parent().remove_child(include);
				}

				bool handle_include(xml_node include, const Uri& base)
				{
					IncludeSpec spec;
					if (!validate_include(include, spec)) return false; // structural: never fallbackable

					// Resolve the href against the directive's effective xml:base.
					Uri ref;
					std::string ref_text = spec.has_href ? spec.href : recompose(base);
					if (!parse_uri(ref_text, ref) || !ref.query.empty() || !ref.fragment.empty())
					{
						fail(xi_error_unsupported_syntax, "include href must not carry a query or fragment: " + ref_text, ref_text);
						return false;
					}

					Uri target = resolve_reference(base, ref);
					std::string target_text = recompose(target);

					// A URI already present in the active reference chain is a cycle.
					for (std::size_t i = 0; i < chain_.size(); ++i)
					{
						if (chain_[i] == target_text)
						{
							fail(xi_error_structural, "circular XInclude reference: " + target_text, target_text);
							return false;
						}
					}

					// xpointer is only valid for XML includes.
					std::vector<int> steps;
					if (spec.has_xpointer)
					{
						if (spec.parse_mode != "xml" || !parse_element_xpointer(spec.xpointer, steps))
						{
							fail(xi_error_structural, "unsupported xpointer syntax: " + spec.xpointer, target_text);
							return false;
						}
					}

					chain_.push_back(target_text);

					Resource resource = resolver_(target_text);
					if (!resource.data)
					{
						if (spec.fallback)
						{
							// Keep the failed URI on the chain while the fallback runs,
							// so a fallback that re-includes the same resource cannot
							// recurse; the pop below removes it afterwards.
							bool recovered = apply_fallback(spec.fallback, include, base);
							chain_.pop_back();
							return recovered;
						}
						chain_.pop_back();
						fail(xi_error_resource_not_found, "resource not found: " + target_text, target_text);
						return false;
					}

					bool ok = false;
					if (spec.parse_mode == "text")
					{
						ok = handle_text(resource, include, spec, target, base);
					}
					else
					{
						ok = handle_xml(resource, include, spec, steps, target, base);
					}

					chain_.pop_back();
					return ok;
				}

				bool handle_xml(const Resource& resource, xml_node include, const IncludeSpec& spec,
				                const std::vector<int>& steps, const Uri& target, const Uri& base)
				{
					xml_document doc;
					xml_parse_result parsed = doc.load_buffer(resource.data, resource.size);
					if (!parsed)
					{
						if (spec.fallback) return apply_fallback(spec.fallback, include, base);
						fail(xi_error_xml_syntax, std::string("XML parse error in ") + recompose(target) + ": " + parsed.description(), recompose(target));
						return false;
					}

					xml_node selected = doc;
					if (!steps.empty() && !select_element(doc, steps, selected))
					{
						if (spec.fallback) return apply_fallback(spec.fallback, include, base);
						fail(xi_error_empty_selection, "xpointer element() selection matched no element: " + spec.xpointer, recompose(target));
						return false;
					}

					// Expand nested includes while the resource document is alive.
					if (!process_tree(selected, target))
					{
						if (fallbackable(error_.code) && spec.fallback)
						{
							error_ = Error();
							return apply_fallback(spec.fallback, include, base);
						}
						return false;
					}

					if (!insert_resource_nodes(selected, include, target, base))
					{
						fail(xi_error_structural, "failed to insert resource nodes", recompose(target));
						return false;
					}
					return true;
				}

				bool handle_text(const Resource& resource, xml_node include, const IncludeSpec& spec,
				                 const Uri& target, const Uri& base)
				{
					const std::uint8_t* bytes = static_cast<const std::uint8_t*>(resource.data);

					std::string text;
					if (!decode_text_resource(bytes, resource.size, text))
					{
						if (spec.fallback) return apply_fallback(spec.fallback, include, base);
						fail(xi_error_text_decoding, "text resource decoding failed: " + recompose(target), recompose(target));
						return false;
					}

					xml_node text_node = include.parent().insert_child_before(node_pcdata, include);
					if (!text_node || !text_node.set_value(text.data(), text.size()))
					{
						fail(xi_error_structural, "failed to insert text node", recompose(target));
						return false;
					}
					include.parent().remove_child(include);
					return true;
				}
			};
		} // namespace

		Result assemble(const xml_document& input, const std::string& document_uri, const ResourceResolver& resolve)
		{
			Assembler assembler(resolve);
			return assembler.run(input, document_uri);
		}
	}
}
