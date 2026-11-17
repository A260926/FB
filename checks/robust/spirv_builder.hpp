// Minimal, dependency-free SPIR-V 1.3 word constructor used by the robust buffer access checks.
// It constructs exactly the small set of logical GLCompute modules the tests need; it is not a general
// SPIR-V assembler. All ids are allocated up front by the test and registered through the builder.
#ifndef ROBUST_SPIRV_BUILDER_HPP
#define ROBUST_SPIRV_BUILDER_HPP

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace robust
{
using Word = uint32_t;

class SpirvBuilder
{
public:
	SpirvBuilder()
	{
		// Magic, version 1.3, generator, bound (patched later), schema.
		words = {0x07230203u, 0x00010300u, 0u, 0u, 0u};
	}

	Word next_id()
	{
		return bound++;
	}

	void op(spv::Op opcode, std::initializer_list<Word> operands)
	{
		words.push_back(((Word)(operands.size() + 1) << 16) | (Word)opcode);
		for (Word w : operands)
			words.push_back(w);
	}

	void raw(spv::Op opcode, const std::vector<Word> &operands)
	{
		words.push_back(((Word)(operands.size() + 1) << 16) | (Word)opcode);
		words.insert(words.end(), operands.begin(), operands.end());
	}

	// Packs a NUL-terminated string into one or more little-endian words.
	static std::vector<Word> string_words(const std::string &s)
	{
		std::vector<Word> out;
		size_t bytes = s.size() + 1;
		out.resize((bytes + 3) / 4, 0);
		std::memcpy(out.data(), s.c_str(), bytes);
		return out;
	}

	void name(Word target, const std::string &s)
	{
		auto a = string_words(s);
		std::vector<Word> operands;
		operands.push_back(target);
		operands.insert(operands.end(), a.begin(), a.end());
		raw(spv::OpName, operands);
	}

	void member_name(Word struct_type, Word member, const std::string &s)
	{
		auto a = string_words(s);
		std::vector<Word> operands;
		operands.push_back(struct_type);
		operands.push_back(member);
		operands.insert(operands.end(), a.begin(), a.end());
		raw(spv::OpMemberName, operands);
	}

	void decorate(Word target, spv::Decoration decoration, std::initializer_list<Word> extra = {})
	{
		std::vector<Word> operands = {target, (Word)decoration};
		operands.insert(operands.end(), extra.begin(), extra.end());
		raw(spv::OpDecorate, operands);
	}

	void member_decorate(Word struct_type, Word member, spv::Decoration decoration,
	                     std::initializer_list<Word> extra = {})
	{
		std::vector<Word> operands = {struct_type, member, (Word)decoration};
		operands.insert(operands.end(), extra.begin(), extra.end());
		raw(spv::OpMemberDecorate, operands);
	}

	std::vector<Word> finish()
	{
		words[3] = bound;
		return words;
	}

	Word bound = 1;
	std::vector<Word> words;
};
} // namespace robust

#endif
