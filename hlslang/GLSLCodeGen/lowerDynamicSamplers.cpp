// Copyright (c) The HLSL2GLSLFork Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.txt file.

#include "lowerDynamicSamplers.h"

#include "localintermediate.h"

#include <set>
#include <string>

namespace
{

/// Texture built-ins whose direct sampler argument may be a dynamically
/// indexed global sampler2D array.
bool IsSupportedDynamicSamplerOp(TOperator op, int argCount)
{
	switch (op)
	{
	case EOpTex2D:
		// Two-argument tex2D (implicit LOD) and the four-argument form
		// (explicit gradients) share the same operator.
		return argCount == 2 || argCount == 4;
	case EOpTex2DBias:
	case EOpTex2DLod:
		return argCount == 2;
	default:
		return false;
	}
}

bool IsSamplerBasicType(TBasicType t)
{
	switch (t)
	{
	case EbtSamplerGeneric:
	case EbtSampler1D:
	case EbtSampler1DShadow:
	case EbtSampler2D:
	case EbtSampler2DShadow:
	case EbtSampler3D:
	case EbtSamplerCube:
	case EbtSamplerRect:
	case EbtSamplerRectShadow:
	case EbtSampler2DArray:
		return true;
	default:
		return false;
	}
}

const char* TextureOpName(TOperator op)
{
	switch (op)
	{
	case EOpTex1D: return "tex1D";
	case EOpTex1DProj: return "tex1Dproj";
	case EOpTex1DLod: return "tex1Dlod";
	case EOpTex1DBias: return "tex1Dbias";
	case EOpTex1DGrad: return "tex1Dgrad";
	case EOpTex2D: return "tex2D";
	case EOpTex2DProj: return "tex2Dproj";
	case EOpTex2DLod: return "tex2Dlod";
	case EOpTex2DBias: return "tex2Dbias";
	case EOpTex2DGrad: return "tex2Dgrad";
	case EOpTex3D: return "tex3D";
	case EOpTex3DProj: return "tex3Dproj";
	case EOpTex3DLod: return "tex3Dlod";
	case EOpTex3DBias: return "tex3Dbias";
	case EOpTex3DGrad: return "tex3Dgrad";
	case EOpTexCube: return "texCUBE";
	case EOpTexCubeProj: return "texCUBEproj";
	case EOpTexCubeLod: return "texCUBElod";
	case EOpTexCubeBias: return "texCUBEbias";
	case EOpTexCubeGrad: return "texCUBEgrad";
	case EOpTexRect: return "texRECT";
	case EOpTexRectProj: return "texRECTproj";
	case EOpShadow2D: return "shadow2D";
	case EOpShadow2DProj: return "shadow2Dproj";
	case EOpTex2DArray: return "tex2DArray";
	case EOpTex2DArrayLod: return "tex2DArrayLod";
	case EOpTex2DArrayBias: return "tex2DArrayBias";
	default: return "texture built-in";
	}
}

bool IsAnyTextureOp(TOperator op)
{
	return op >= EOpTex1D && op <= EOpTex2DArrayBias;
}

TIntermConstant* MakeIntConstant(int value, const TSourceLoc& line)
{
	TIntermConstant* node = ir_add_constant(TType(EbtInt, EbpUndefined, EvqConst), line);
	node->setValue(value);
	return node;
}

TIntermConstant* MakeVec4Zero(const TType& vec4Type, const TSourceLoc& line)
{
	TIntermConstant* node = ir_add_constant(vec4Type, line);
	for (unsigned i = 0; i < node->getCount(); ++i)
		node->setValue(i, 0.0f);
	return node;
}

struct TDynamicSamplerLowerer
{
	explicit TDynamicSamplerLowerer(TInfoSink& sink)
		: infoSink(sink), failed(false), nextSyntheticId(-1), helperCounter(0)
	{
	}

	bool Run(TIntermNode* root);

private:
	TIntermNode* LowerNode(TIntermNode* node);
	TIntermTyped* LowerTyped(TIntermTyped* node);
	TIntermAggregate* LowerTextureCall(TIntermAggregate* node);
	TIntermAggregate* BuildHelper(const TType& returnType, TIntermSymbol* arraySymbol, int arraySize,
	                              TOperator texOp, const TNodeArray& callArgs, const TSourceLoc& line,
	                              std::string& helperName);
	TIntermSymbol* MakeSyntheticSymbol(const std::string& name, const TType& type);
	std::string MintName(const std::string& base);
	TIntermSymbol* MakeArrayReference(TIntermSymbol* arraySymbol, const TSourceLoc& line);
	TIntermBinary* MakeConstantArrayIndex(TIntermSymbol* arraySymbol, const TType& elementType,
	                                      int index, const TSourceLoc& line);
	TIntermTyped* EnsureIntIndex(TIntermTyped* index, const TSourceLoc& line);
	void CollectNames(TIntermNode* node);
	void ReportError(const TSourceLoc& line, const char* message);

	TInfoSink& infoSink;
	bool failed;
	int nextSyntheticId;
	int helperCounter;
	std::set<std::string> usedIdentifiers;
	std::vector<TIntermAggregate*> generatedHelpers;
};

void TDynamicSamplerLowerer::ReportError(const TSourceLoc& line, const char* message)
{
	failed = true;
	infoSink.info << "Error: " << line << ": " << message << "\n";
}

TIntermSymbol* TDynamicSamplerLowerer::MakeSyntheticSymbol(const std::string& name, const TType& type)
{
	// Negative ids can never collide with parser-assigned symbol ids.
	TIntermSymbol* node = new TIntermSymbol(nextSyntheticId--, TString(name.c_str()), type);
	return node;
}

std::string TDynamicSamplerLowerer::MintName(const std::string& base)
{
	std::string name;
	do
	{
		std::stringstream ss;
		ss << base << helperCounter++;
		name = ss.str();
	}
	while (usedIdentifiers.count(name) != 0);
	usedIdentifiers.insert(name);
	return name;
}

TIntermSymbol* TDynamicSamplerLowerer::MakeArrayReference(TIntermSymbol* arraySymbol, const TSourceLoc& line)
{
	// Reuse the original id so the GLSL back end shares the uniform symbol
	// and keeps the original name, array size and reflection entry.
	TIntermSymbol* ref = new TIntermSymbol(arraySymbol->getId(), arraySymbol->getSymbol(),
	                                       arraySymbol->getType());
	ref->setGlobal(arraySymbol->isGlobal());
	ref->setLine(line);
	return ref;
}

TIntermBinary* TDynamicSamplerLowerer::MakeConstantArrayIndex(TIntermSymbol* arraySymbol,
                                                              const TType& elementType,
                                                              int index, const TSourceLoc& line)
{
	TIntermBinary* node = new TIntermBinary(EOpIndexDirect);
	node->setLeft(MakeArrayReference(arraySymbol, line));
	node->setRight(MakeIntConstant(index, line));
	node->setType(elementType);
	node->setLine(line);
	return node;
}

TIntermTyped* TDynamicSamplerLowerer::EnsureIntIndex(TIntermTyped* index, const TSourceLoc& line)
{
	if (index->getBasicType() == EbtInt)
		return index;

	if (index->getBasicType() == EbtFloat)
	{
		TType intType(EbtInt, EbpUndefined, EvqTemporary);
		TIntermUnary* converted = new TIntermUnary(EOpConvFloatToInt, intType);
		converted->setOperand(index);
		converted->setLine(line);
		return converted;
	}

	if (index->getBasicType() == EbtBool)
	{
		TType intType(EbtInt, EbpUndefined, EvqTemporary);
		TIntermUnary* converted = new TIntermUnary(EOpConvBoolToInt, intType);
		converted->setOperand(index);
		converted->setLine(line);
		return converted;
	}

	ReportError(line, "dynamic sampler array index must be an integer expression");
	return index;
}

TIntermAggregate* TDynamicSamplerLowerer::BuildHelper(const TType& returnType,
                                                      TIntermSymbol* arraySymbol, int arraySize,
                                                      TOperator texOp, const TNodeArray& callArgs,
                                                      const TSourceLoc& line,
                                                      std::string& helperName)
{
	// callArgs: [dynamic index, coord, remaining texture arguments ...]
	helperName = MintName("xlat_dynsampler2D_");

	// Helper parameters.  Every call-site argument is evaluated exactly once
	// and forwarded in source order: index, coordinate, then the rest.
	std::vector<TIntermSymbol*> params;
	{
		TType indexType(EbtInt, EbpUndefined, EvqIn);
		params.push_back(MakeSyntheticSymbol(MintName(helperName + "_index_"), indexType));
	}
	for (size_t i = 1; i < callArgs.size(); ++i)
	{
		TType paramType = callArgs[i]->getAsTyped()->getType();
		paramType.changeQualifier(EvqIn);
		std::stringstream ss;
		ss << helperName << "_arg" << (i - 1) << "_";
		params.push_back(MakeSyntheticSymbol(MintName(ss.str()), paramType));
	}

	TIntermAggregate* paramList = new TIntermAggregate(EOpParameters);
	for (size_t i = 0; i < params.size(); ++i)
		paramList->getNodes().push_back(params[i]);

	TIntermAggregate* body = new TIntermAggregate(EOpSequence);
	body->setLine(line);

	const TType& elementType = callArgs[0]->getAsTyped()->getType();

	// One temporary per array element.  All candidate samples run
	// unconditionally with constant indices, so implicit-LOD calls stay in
	// uniform control flow; bias, explicit LOD and gradients are forwarded
	// verbatim through the original operator nodes.
	std::vector<TIntermSymbol*> candidates;
	for (int k = 0; k < arraySize; ++k)
	{
		TIntermAggregate* sample = new TIntermAggregate(texOp);
		sample->getNodes().push_back(MakeConstantArrayIndex(arraySymbol, elementType, k, line));
		for (size_t a = 1; a < params.size(); ++a)
			sample->getNodes().push_back(params[a]);
		sample->setType(returnType);
		sample->setLine(line);

		std::stringstream nameStream;
		nameStream << helperName << "_c" << k << "_";
		TIntermSymbol* temp = MakeSyntheticSymbol(MintName(nameStream.str()), returnType);
		TIntermBinary* assignment = new TIntermBinary(EOpAssign);
		assignment->setLeft(temp);
		assignment->setRight(sample);
		assignment->setType(returnType);
		assignment->setLine(line);

		TIntermDeclaration* declaration = new TIntermDeclaration(returnType);
		declaration->getDeclaration() = assignment;
		declaration->setLine(line);
		body->getNodes().push_back(declaration);
		candidates.push_back(temp);
	}

	// Only the final color selection is branched; out-of-range indices
	// resolve to a zero color while every candidate sample still ran.
	TIntermTyped* selection = MakeVec4Zero(returnType, line);
	for (int k = arraySize - 1; k >= 0; --k)
	{
		TIntermBinary* equals = new TIntermBinary(EOpEqual);
		equals->setLeft(params[0]);
		equals->setRight(MakeIntConstant(k, line));
		equals->setType(TType(EbtBool, EbpUndefined, EvqTemporary));
		equals->setLine(line);

		TIntermSelection* ternary = new TIntermSelection(equals, candidates[k], selection, returnType);
		ternary->setLine(line);
		selection = ternary;
	}

	body->getNodes().push_back(ir_add_branch(EOpReturn, selection, line));

	TIntermAggregate* function = new TIntermAggregate(EOpFunction);
	function->getNodes().push_back(paramList);
	function->getNodes().push_back(body);
	function->setName(TString(helperName.c_str()));
	function->setPlainName(TString(helperName.c_str()));
	function->setType(returnType);
	function->setLine(line);
	return function;
}

TIntermAggregate* TDynamicSamplerLowerer::LowerTextureCall(TIntermAggregate* node)
{
	const TSourceLoc line = node->getLine();
	TNodeArray& args = node->getNodes();
	TIntermBinary* samplerIndex = args[0]->getAsBinaryNode();

	if (!samplerIndex || samplerIndex->getOp() != EOpIndexIndirect)
		return node; // constant or otherwise already supported

	if (!IsSamplerBasicType(samplerIndex->getBasicType()))
		return node;

	if (!IsSupportedDynamicSamplerOp(node->getOp(), (int)args.size()))
	{
		if (IsAnyTextureOp(node->getOp()))
		{
			std::stringstream ss;
			ss << "dynamic sampler arrays are not supported as arguments of "
			   << TextureOpName(node->getOp());
			ReportError(line, ss.str().c_str());
		}
		else
		{
			ReportError(line, "dynamic sampler arrays are only supported as direct sampler "
			                  "arguments of tex2D, tex2Dbias, tex2Dlod and four-argument tex2D");
		}
		return node;
	}

	TIntermSymbol* arraySymbol = samplerIndex->getLeft()->getAsSymbolNode();
	if (!arraySymbol)
	{
		ReportError(samplerIndex->getLine(),
		            "dynamic sampler indexing is only supported on a global sampler2D array symbol");
		return node;
	}

	const TType& arrayType = arraySymbol->getType();
	if (!arraySymbol->isGlobal() || arrayType.getQualifier() != EvqUniform ||
	    !arrayType.isArray() || arrayType.getArraySize() <= 0)
	{
		ReportError(samplerIndex->getLine(),
		            "dynamic sampler indexing requires a global fixed-length uniform sampler2D array");
		return node;
	}

	if (arrayType.getBasicType() != EbtSampler2D)
	{
		ReportError(samplerIndex->getLine(),
		            "dynamic sampler lowering only supports sampler2D arrays");
		return node;
	}

	// Lower sub-expressions first so nested dynamic samples keep their
	// source evaluation order.
	TNodeArray loweredArgs;
	loweredArgs.reserve(args.size());
	loweredArgs.push_back(EnsureIntIndex(LowerTyped(samplerIndex->getRight()), line));
	for (size_t i = 1; i < args.size(); ++i)
		loweredArgs.push_back(LowerTyped(args[i]->getAsTyped()));

	std::string helperName;
	TIntermAggregate* helper = BuildHelper(node->getType(), arraySymbol, arrayType.getArraySize(),
	                                       node->getOp(), loweredArgs, line, helperName);
	generatedHelpers.push_back(helper);

	TIntermAggregate* call = new TIntermAggregate(EOpFunctionCall);
	call->setName(TString(helperName.c_str()));
	call->setPlainName(TString(helperName.c_str()));
	for (size_t i = 0; i < loweredArgs.size(); ++i)
		call->getNodes().push_back(loweredArgs[i]);
	call->setType(node->getType());
	call->setLine(line);
	return call;
}

TIntermTyped* TDynamicSamplerLowerer::LowerTyped(TIntermTyped* node)
{
	TIntermNode* lowered = LowerNode(node);
	return lowered ? lowered->getAsTyped() : 0;
}

TIntermNode* TDynamicSamplerLowerer::LowerNode(TIntermNode* node)
{
	if (!node || failed)
		return node;

	if (TIntermAggregate* aggregate = node->getAsAggregate())
	{
		const TOperator op = aggregate->getOp();

		if (op == EOpSequence)
		{
			TNodeArray& children = aggregate->getNodes();
			for (size_t i = 0; i < children.size(); ++i)
				children[i] = LowerNode(children[i]);
			return aggregate;
		}

		if (op == EOpFunction)
		{
			TNodeArray& children = aggregate->getNodes();
			// [0] is the EOpParameters list, [1] is the function body.
			for (size_t i = 1; i < children.size(); ++i)
				children[i] = LowerNode(children[i]);
			return aggregate;
		}

		if (IsAnyTextureOp(op) && !aggregate->getNodes().empty())
		{
			TIntermNode* samplerArg = aggregate->getNodes()[0];
			TIntermBinary* binary = samplerArg ? samplerArg->getAsBinaryNode() : 0;
			if (binary && binary->getOp() == EOpIndexIndirect &&
			    IsSamplerBasicType(binary->getBasicType()))
			{
				return LowerTextureCall(aggregate);
			}
		}

		if (op == EOpFunctionCall)
		{
			TNodeArray& children = aggregate->getNodes();
			for (size_t i = 0; i < children.size(); ++i)
			{
				TIntermBinary* binary = children[i] ? children[i]->getAsBinaryNode() : 0;
				if (binary && binary->getOp() == EOpIndexIndirect &&
				    IsSamplerBasicType(binary->getBasicType()))
				{
					ReportError(binary->getLine(),
					            "a dynamically indexed sampler cannot escape into a function argument");
					return aggregate;
				}
				children[i] = LowerNode(children[i]);
			}
			return aggregate;
		}

		TNodeArray& children = aggregate->getNodes();
		for (size_t i = 0; i < children.size(); ++i)
			children[i] = LowerNode(children[i]);
		return aggregate;
	}

	if (TIntermBinary* binary = node->getAsBinaryNode())
	{
		if (binary->getOp() == EOpIndexIndirect && IsSamplerBasicType(binary->getBasicType()))
		{
			ReportError(binary->getLine(),
			            "dynamic sampler array indexing is only allowed as the direct sampler "
			            "argument of tex2D, tex2Dbias, tex2Dlod and four-argument tex2D");
			return node;
		}

		binary->setLeft(LowerTyped(binary->getLeft()));
		binary->setRight(LowerTyped(binary->getRight()));
		return binary;
	}

	if (TIntermUnary* unary = node->getAsUnaryNode())
	{
		unary->setOperand(LowerTyped(unary->getOperand()));
		return unary;
	}

	if (TIntermSelection* selection = node->getAsSelectionNode())
	{
		if (TIntermTyped* condition = selection->getCondition()->getAsTyped())
			selection->setCondition(LowerTyped(condition));
		if (selection->getTrueBlock())
			selection->setTrueBlock(LowerNode(selection->getTrueBlock()));
		if (selection->getFalseBlock())
			selection->setFalseBlock(LowerNode(selection->getFalseBlock()));
		return selection;
	}

	if (TIntermLoop* loop = node->getAsLoopNode())
	{
		if (loop->getCondition())
			loop->setCondition(LowerTyped(loop->getCondition()));
		if (loop->getBody())
			loop->setBody(LowerNode(loop->getBody()));
		if (loop->getExpression())
			loop->setExpression(LowerTyped(loop->getExpression()));
		return loop;
	}

	if (TIntermBranch* branch = node->getAsBranchNode())
	{
		if (branch->getExpression())
			branch->setExpression(LowerTyped(branch->getExpression()));
		return branch;
	}

	if (TIntermDeclaration* declaration = node->getAsDeclaration())
	{
		LowerNode(declaration->getDeclaration());
		return declaration;
	}

	return node;
}

void TDynamicSamplerLowerer::CollectNames(TIntermNode* root)
{
	struct TNameCollector : public TIntermTraverser
	{
		explicit TNameCollector(std::set<std::string>& names) : usedNames(names)
		{
			visitSymbol = CollectSymbol;
			visitAggregate = CollectAggregate;
		}

		static void CollectSymbol(TIntermSymbol* symbol, TIntermTraverser* traverser)
		{
			TNameCollector* self = static_cast<TNameCollector*>(traverser);
			self->usedNames.insert(symbol->getSymbol().c_str());
		}

		static bool CollectAggregate(bool preVisit, TIntermAggregate* aggregate, TIntermTraverser* traverser)
		{
			if (!preVisit)
				return true;
			TNameCollector* self = static_cast<TNameCollector*>(traverser);
			if (aggregate->getOp() == EOpFunction)
			{
				self->usedNames.insert(aggregate->getName().c_str());
				self->usedNames.insert(aggregate->getPlainName().c_str());
			}
			return true;
		}

		std::set<std::string>& usedNames;
	};

	TNameCollector collector(usedIdentifiers);
	root->traverse(&collector);
}

bool TDynamicSamplerLowerer::Run(TIntermNode* root)
{
	CollectNames(root);
	LowerNode(root);

	if (!failed && !generatedHelpers.empty())
	{
		TIntermAggregate* topLevel = root->getAsAggregate();
		if (!topLevel)
		{
			TSourceLoc loc = gNullSourceLoc;
			ReportError(loc, "internal error: unexpected shader root node");
			return false;
		}
		for (size_t i = 0; i < generatedHelpers.size(); ++i)
			topLevel->getNodes().push_back(generatedHelpers[i]);
	}

	return !failed;
}

} // namespace

bool LowerDynamicSamplers(TIntermNode* root, TInfoSink& infoSink)
{
	TDynamicSamplerLowerer lowerer(infoSink);
	return lowerer.Run(root);
}
