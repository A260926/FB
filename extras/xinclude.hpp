/**
 * XInclude assembly helper for pugixml
 * --------------------------------------------------------
 * SPDX-License-Identifier: MIT
 *
 * This header provides an explicit, side-effect free XInclude assembly entry
 * point. The core pugixml parser is not modified: load_string/load_buffer,
 * XPath queries and serialization keep their default behavior; an XInclude
 * element is only expanded when assemble() is called.
 *
 * The implementation never touches the file system or the network. Every
 * referenced resource is obtained through a user supplied resolver callback,
 * which receives an absolute URI with all "." / ".." path segments removed.
 *
 * Supported subset (see extras/xinclude.cpp for details):
 *  - xi:include / xi:fallback bound to http://www.w3.org/2001/XInclude
 *    (namespace identity is used, never the prefix spelling);
 *  - parse="xml" (default) and parse="text";
 *  - no xpointer, or xpointer="element(/1/2/...)" with 1-based element steps;
 *  - href resolution against the effective xml:base of each include, with
 *    nested includes, fallback processing and include-cycle detection;
 *  - text resources in UTF-8 and UTF-16LE/UTF-16BE only.
 */

#ifndef HEADER_PUGIXML_XINCLUDE_HPP
#define HEADER_PUGIXML_XINCLUDE_HPP

#include "pugixml.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pugi
{
	namespace xinclude
	{
		// Namespace URI that identifies XInclude elements.
		extern const char* const XINCLUDE_NAMESPACE;

		// Reserved namespace of the xml: prefix.
		extern const char* const XML_NAMESPACE;

		// Raw bytes of one resource, as returned by the resolver.
		// A null data pointer means that the resource does not exist;
		// a non-null pointer with size 0 is a valid empty resource.
		struct Resource
		{
			const void* data;
			std::size_t size;

			Resource(): data(PUGIXML_NULL), size(0)
			{
			}

			Resource(const void* data_, std::size_t size_): data(data_), size(size_)
			{
			}

			static Resource not_found()
			{
				return Resource();
			}
		};

		// Resolver callback.
		// Argument: absolute, dot-segment-normalized URI of the resource.
		// Result:   the resource bytes, or Resource::not_found().
		// The callback is never asked to perform percent decoding and it must
		// not throw to signal "not found" (return the not_found value instead).
		typedef std::function<Resource(const std::string& absolute_uri)> ResourceResolver;

		// Classification of an assembly failure.
		enum ErrorCode
		{
			xi_error_none = 0,               // No error.
			xi_error_invalid_uri,            // Base/href URI cannot be parsed or resolved.
			xi_error_unsupported_syntax,     // Query/fragment URI, unknown xpointer, etc.
			xi_error_resource_not_found,     // Resolver reported a missing resource.
			xi_error_xml_syntax,             // Referenced XML document is not well formed.
			xi_error_empty_selection,        // element() xpointer matched nothing.
			xi_error_text_decoding,          // Text resource could not be decoded.
			xi_error_structural              // Malformed include/fallback or include cycle.
		};

		// Diagnostic information for a failed assembly.
		struct Error
		{
			// Machine readable failure class (xi_error_*).
			ErrorCode code;

			// Human readable explanation.
			std::string message;

			// Absolute URI of the include that could not be satisfied.
			std::string resource_uri;

			// Reference chain from the main document to the failing include,
			// starting with the main document URI; the last entry is the
			// failing resource URI (resource_uri duplicates it).
			std::vector<std::string> include_chain;

			Error(): code(xi_error_none)
			{
			}
		};

		// Result of assemble(): on success the document owns the fully expanded
		// tree; on failure the document is empty and Error describes the problem.
		// The object is movable but not copyable (it embeds xml_document).
		struct Result
		{
			// Expanded, self-contained document; valid when the result is true.
			xml_document document;

			// Failure diagnostic; empty when assembly succeeded.
			Error error;

			// True when assembly succeeded.
			operator bool() const
			{
				return error.code == xi_error_none;
			}
		};

		// Assemble a new document by expanding all XInclude directives of input.
		//
		// input:        read-only main document; it is never modified and no node
		//               handle is moved out of it.
		// document_uri: absolute URI of the main document; the initial xml:base.
		// resolve:      resource callback, invoked only with absolute normalized
		//               URIs. The implementation itself performs no I/O.
		//
		// Returns a Result containing an independent xml_document (the returned
		// tree does not depend on the input document or any temporary resource
		// document lifetime) plus an Error diagnostic.
		Result assemble(const xml_document& input, const std::string& document_uri, const ResourceResolver& resolve);
	}
}

#endif
