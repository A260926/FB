/**
 * GLSL ES symbol scoping for effect shader composition.
 *
 * This module analyses the top-level declarations of an effect shader - named structures, global variables,
 * constants, function declarations and function definitions - and renames them with a unique prefix so that
 * multiple effects can share one compound shader without symbol collisions. References inside the effect
 * source are rewritten to match the renamed declarations while local variables, function parameters, loop
 * variables, structure members and identifiers inside comments keep their original meaning.
 *
 * Only symbols that are declared directly in the effect source are renamed. Built-in identifiers and
 * functions provided by the surrounding material template (e.g. readDepth) are left untouched.
 *
 * @internal
 */

const TOKEN_EOF = "eof";
const TOKEN_IDENTIFIER = "identifier";
const TOKEN_NUMBER = "number";
const TOKEN_PUNCTUATION = "punctuation";
const TOKEN_DIRECTIVE = "directive";
const TOKEN_COMMENT = "comment";
const TOKEN_WHITESPACE = "whitespace";

/**
 * A token that marks the end of the input.
 *
 * @type {Object}
 */

const EOF = Object.freeze({ type: TOKEN_EOF, value: "" });

/**
 * Built-in GLSL ES value types.
 *
 * @type {Set<String>}
 */

const BUILTIN_TYPES = new Set([
	"void", "bool", "int", "uint", "float",
	"vec2", "vec3", "vec4",
	"bvec2", "bvec3", "bvec4",
	"ivec2", "ivec3", "ivec4",
	"uvec2", "uvec3", "uvec4",
	"mat2", "mat3", "mat4",
	"mat2x2", "mat2x3", "mat2x4",
	"mat3x2", "mat3x3", "mat3x4",
	"mat4x2", "mat4x3", "mat4x4",
	"sampler2D", "samplerCube", "sampler3D", "sampler2DArray",
	"sampler2DShadow", "samplerCubeShadow", "sampler2DArrayShadow"
]);

/**
 * Declaration qualifiers that may precede a type.
 *
 * @type {Set<String>}
 */

const QUALIFIERS = new Set([
	"const", "uniform", "varying", "attribute", "invariant",
	"in", "out", "inout",
	"lowp", "mediump", "highp",
	"centroid", "flat", "smooth", "noperspective", "sample"
]);

/**
 * Single character punctuation tokens.
 *
 * @type {Set<String>}
 */

const SINGLE_CHAR_PUNCTUATION = new Set([..."(){}[];,.:?~!+-*/%<>=&|^#"]);

/**
 * Multi character punctuation tokens.
 *
 * @type {Set<String>}
 */

const MULTI_CHAR_PUNCTUATION = new Set([
	"<<=", ">>=",
	"++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^",
	"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##"
]);

/**
 * A pattern for macro definition keys with an optional parameter list.
 *
 * @type {RegExp}
 */

const DEFINE_KEY = /^([A-Za-z_]\w*)(\(([^)]*)\))?$/;

/**
 * Checks whether the given character is a decimal digit.
 *
 * @param {String} c - A character.
 * @return {Boolean} Whether the character is a digit.
 */

function isDigit(c) {

	return (c >= "0" && c <= "9");

}

/**
 * Checks whether the given character can start an identifier.
 *
 * @param {String} c - A character.
 * @return {Boolean} Whether the character can start an identifier.
 */

function isAlpha(c) {

	return (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || c === "_";

}

/**
 * Checks whether the given character can be part of an identifier.
 *
 * @param {String} c - A character.
 * @return {Boolean} Whether the character can be part of an identifier.
 */

function isAlphaNum(c) {

	return isAlpha(c) || isDigit(c);

}

/**
 * Checks whether the given character is a hexadecimal digit.
 *
 * @param {String} c - A character.
 * @return {Boolean} Whether the character is a hexadecimal digit.
 */

function isHexDigit(c) {

	return isDigit(c) || (c >= "a" && c <= "f") || (c >= "A" && c <= "F");

}

/**
 * Checks whether the given character is whitespace.
 *
 * @param {String} c - A character.
 * @return {Boolean} Whether the character is whitespace.
 */

function isWhitespace(c) {

	return c === " " || c === "\t" || c === "\r" || c === "\n" || c === "\f" || c === "\v";

}

/**
 * Splits GLSL source code into tokens.
 *
 * Whitespace and comments are preserved as tokens so that the source can be reassembled unchanged.
 * Preprocessor directives are returned as single tokens if `allowDirectives` is enabled.
 *
 * @private
 * @param {String} source - The source code.
 * @param {Boolean} [allowDirectives=true] - Whether preprocessor directives should be treated as single tokens.
 * @return {Object[]} The tokens.
 */

function tokenize(source, allowDirectives = true) {

	const tokens = [];
	const length = source.length;
	let i = 0;

	while(i < length) {

		const c = source[i];

		if(isWhitespace(c)) {

			let j = i + 1;
			while(j < length && isWhitespace(source[j])) j++;
			tokens.push({ type: TOKEN_WHITESPACE, value: source.slice(i, j) });
			i = j;
			continue;

		}

		if(c === "/" && source[i + 1] === "/") {

			let j = i + 2;
			while(j < length && source[j] !== "\n") j++;
			tokens.push({ type: TOKEN_COMMENT, value: source.slice(i, j) });
			i = j;
			continue;

		}

		if(c === "/" && source[i + 1] === "*") {

			const end = source.indexOf("*/", i + 2);
			const j = (end === -1) ? length : end + 2;
			tokens.push({ type: TOKEN_COMMENT, value: source.slice(i, j) });
			i = j;
			continue;

		}

		if(c === "#" && allowDirectives) {

			// A directive runs to the end of the line, honouring line continuations.
			let j = i + 1;

			while(j < length) {

				if(source[j] === "\n") {

					let k = j - 1;
					while(k > i && source[k] === "\r") k--;
					if(source[k] !== "\\") break;
					j++;

				} else {

					j++;

				}

			}

			tokens.push({ type: TOKEN_DIRECTIVE, value: source.slice(i, j) });
			i = j;
			continue;

		}

		if(isAlpha(c)) {

			let j = i + 1;
			while(j < length && isAlphaNum(source[j])) j++;
			tokens.push({ type: TOKEN_IDENTIFIER, value: source.slice(i, j) });
			i = j;
			continue;

		}

		if(isDigit(c) || (c === "." && isDigit(source[i + 1]))) {

			let j = i;

			if(c === "0" && (source[i + 1] === "x" || source[i + 1] === "X")) {

				j = i + 2;
				while(j < length && isHexDigit(source[j])) j++;

			} else {

				while(j < length && isDigit(source[j])) j++;

				if(source[j] === ".") {

					j++;
					while(j < length && isDigit(source[j])) j++;

				}

				if(source[j] === "e" || source[j] === "E") {

					let k = j + 1;
					if(source[k] === "+" || source[k] === "-") k++;

					if(isDigit(source[k])) {

						j = k + 1;
						while(j < length && isDigit(source[j])) j++;

					}

				}

			}

			tokens.push({ type: TOKEN_NUMBER, value: source.slice(i, j) });
			i = j;
			continue;

		}

		const three = source.slice(i, i + 3);

		if(three === "<<=" || three === ">>=") {

			tokens.push({ type: TOKEN_PUNCTUATION, value: three });
			i += 3;
			continue;

		}

		const two = source.slice(i, i + 2);

		if(MULTI_CHAR_PUNCTUATION.has(two)) {

			tokens.push({ type: TOKEN_PUNCTUATION, value: two });
			i += 2;
			continue;

		}

		if(SINGLE_CHAR_PUNCTUATION.has(c)) {

			tokens.push({ type: TOKEN_PUNCTUATION, value: c });
			i += 1;
			continue;

		}

		throw new Error(`GLSLScoper: unexpected character "${c}" at offset ${i}`);

	}

	return tokens;

}

/**
 * Extracts the parameter names of a function-like macro.
 *
 * @private
 * @param {String} paramsText - The parameter list including the parentheses, or undefined.
 * @return {Set<String>} The parameter names, or null if there is no parameter list.
 */

function macroParameters(paramsText) {

	if(paramsText === undefined) {

		return null;

	}

	return new Set(paramsText.slice(1, -1).split(",").map((x) => x.trim()).filter((x) => x.length > 0));

}

/**
 * A recursive descent parser that renames the top-level symbols of one GLSL translation unit.
 *
 * The parser tracks local scopes to detect shadowing: identifiers that refer to local variables, function
 * parameters or loop variables are never renamed. Identifiers that follow a member selector are not treated
 * as global references either.
 *
 * @private
 */

class Parser {

	/**
	 * Constructs a new parser.
	 *
	 * @param {GLSLScoper} scoper - The scoper that owns the global symbol table.
	 * @param {Object[]} tokens - The tokens to parse.
	 */

	constructor(scoper, tokens) {

		/**
		 * The scoper that owns the global symbol table.
		 *
		 * @type {GLSLScoper}
		 * @private
		 */

		this.scoper = scoper;

		/**
		 * The tokens.
		 *
		 * @type {Object[]}
		 * @private
		 */

		this.tokens = tokens;

		/**
		 * The position of the next unconsumed token.
		 *
		 * @type {Number}
		 * @private
		 */

		this.pos = 0;

		/**
		 * The last consumed significant token.
		 *
		 * @type {Object}
		 * @private
		 */

		this.prevToken = null;

		/**
		 * The stack of local scopes. Each scope holds the names of local variables and local type names.
		 *
		 * @type {Object[]}
		 * @private
		 */

		this.scopes = [];

	}

	/**
	 * Returns the index of the next significant token, starting at the given index.
	 *
	 * Whitespace and comments are skipped. Preprocessor directives are renamed as a side effect and skipped.
	 *
	 * @private
	 * @param {Number} start - The index to start at.
	 * @return {Number} The index of the next significant token.
	 */

	scanSignificant(start) {

		let i = start;

		while(i < this.tokens.length) {

			const token = this.tokens[i];

			if(token.type === TOKEN_WHITESPACE || token.type === TOKEN_COMMENT) {

				i++;
				continue;

			}

			if(token.type === TOKEN_DIRECTIVE) {

				this.scoper.processDirective(token);
				i++;
				continue;

			}

			break;

		}

		return i;

	}

	/**
	 * Returns a significant token without consuming it.
	 *
	 * @private
	 * @param {Number} [ahead=0] - How many significant tokens to look ahead.
	 * @return {Object} The token.
	 */

	peek(ahead = 0) {

		let i = this.scanSignificant(this.pos);

		for(let n = 0; n < ahead && i < this.tokens.length; n++) {

			i = this.scanSignificant(i + 1);

		}

		return (i < this.tokens.length) ? this.tokens[i] : EOF;

	}

	/**
	 * Consumes and returns the next significant token.
	 *
	 * @private
	 * @return {Object} The token.
	 */

	advance() {

		const i = this.scanSignificant(this.pos);
		const token = (i < this.tokens.length) ? this.tokens[i] : EOF;
		this.pos = (i < this.tokens.length) ? i + 1 : i;

		if(token !== EOF) {

			this.prevToken = token;

		}

		return token;

	}

	/**
	 * Consumes the next significant token and checks its value.
	 *
	 * @private
	 * @param {String} value - The expected token value.
	 * @return {Object} The token.
	 */

	expect(value) {

		const token = this.advance();

		if(token.value !== value) {

			throw new Error(`GLSLScoper: expected "${value}", found "${token.value}"`);

		}

		return token;

	}

	/**
	 * Pushes a new local scope onto the scope stack.
	 *
	 * @private
	 */

	pushScope() {

		this.scopes.push({ names: new Set(), types: new Set() });

	}

	/**
	 * Removes the innermost local scope.
	 *
	 * @private
	 */

	popScope() {

		this.scopes.pop();

	}

	/**
	 * Checks whether the given name refers to a local variable or local type.
	 *
	 * @private
	 * @param {String} name - The name.
	 * @return {Boolean} Whether the name is local.
	 */

	isLocalName(name) {

		for(const scope of this.scopes) {

			if(scope.names.has(name) || scope.types.has(name)) {

				return true;

			}

		}

		return false;

	}

	/**
	 * Checks whether the given name refers to a structure type that is currently in scope.
	 *
	 * @private
	 * @param {String} name - The name.
	 * @return {Boolean} Whether the name is a type name.
	 */

	isTypeName(name) {

		for(let i = this.scopes.length - 1; i >= 0; i--) {

			const scope = this.scopes[i];

			if(scope.types.has(name)) {

				return true;

			}

			if(scope.names.has(name)) {

				return false;

			}

		}

		const global = this.scoper.globals.get(name);
		return (global !== undefined && global.isType);

	}

	/**
	 * Declares a local variable or parameter in the innermost scope. Local names are never renamed.
	 *
	 * @private
	 * @param {Object} token - The name token.
	 */

	declareLocal(token) {

		this.scopes[this.scopes.length - 1].names.add(token.value);

	}

	/**
	 * Declares a local structure type in the innermost scope. Local names are never renamed.
	 *
	 * @private
	 * @param {Object} token - The name token.
	 */

	declareLocalType(token) {

		this.scopes[this.scopes.length - 1].types.add(token.value);

	}

	/**
	 * Declares a global variable and renames its declaration token.
	 *
	 * Interface variables that cannot be renamed safely (attributes, in/out) keep their names.
	 *
	 * @private
	 * @param {Object} token - The name token.
	 * @param {Set<String>} qualifiers - The declaration qualifiers.
	 */

	declareGlobalVariable(token, qualifiers) {

		if(qualifiers.has("attribute") || qualifiers.has("in") || qualifiers.has("out")) {

			// Interface variables with external bindings keep their names.
			return;

		}

		const renamed = this.scoper.declareGlobal(token.value, false);
		token.value = renamed;

		if(qualifiers.has("varying")) {

			this.scoper.varyings.add(renamed);

		}

	}

	/**
	 * Renames the given token if it refers to a global symbol of the current effect.
	 *
	 * @private
	 * @param {Object} token - The identifier token.
	 */

	renameReference(token) {

		const name = token.value;

		if(this.isLocalName(name)) {

			return;

		}

		const renamed = this.scoper.resolveGlobal(name);

		if(renamed !== null) {

			token.value = renamed;

		}

	}

	/**
	 * Parses the translation unit.
	 *
	 * @private
	 */

	parse() {

		while(this.peek() !== EOF) {

			this.parseExternalDeclaration();

		}

	}

	/**
	 * Parses a sequence of declaration qualifiers.
	 *
	 * @private
	 * @return {Set<String>} The qualifiers.
	 */

	parseQualifiers() {

		const qualifiers = new Set();

		while(QUALIFIERS.has(this.peek().value)) {

			qualifiers.add(this.advance().value);

		}

		return qualifiers;

	}

	/**
	 * Parses a single external (top-level) declaration.
	 *
	 * @private
	 */

	parseExternalDeclaration() {

		const token = this.peek();

		if(token.value === ";") {

			this.advance();
			return;

		}

		if(token.value === "precision") {

			this.advance();

			while(this.peek() !== EOF && this.peek().value !== ";") {

				this.advance();

			}

			this.expect(";");
			return;

		}

		const qualifiers = this.parseQualifiers();

		if(this.peek().value === "struct") {

			this.parseStructDeclaration(true, qualifiers);
			return;

		}

		const typeToken = this.advance();

		if(typeToken.type !== TOKEN_IDENTIFIER) {

			throw new Error(`GLSLScoper: expected a type, found "${typeToken.value}"`);

		}

		// Rename structure type usages.
		this.renameReference(typeToken);

		const nameToken = this.advance();

		if(nameToken.type !== TOKEN_IDENTIFIER) {

			throw new Error(`GLSLScoper: expected a name, found "${nameToken.value}"`);

		}

		if(this.peek().value === "(") {

			// A function declaration or definition. Overloads share one renamed symbol.
			nameToken.value = this.scoper.declareGlobal(nameToken.value, false);
			this.advance();
			this.pushScope();
			this.parseParameters();

			if(this.peek().value === ";") {

				// A function declaration without a body.
				this.advance();

			} else {

				this.parseCompoundStatement();

			}

			this.popScope();
			return;

		}

		// A global variable declaration.
		this.parseDeclaratorList(nameToken, (t) => this.declareGlobalVariable(t, qualifiers));

	}

	/**
	 * Parses a structure declaration, including optional variable declarators after the body.
	 *
	 * @private
	 * @param {Boolean} global - Whether the declaration appears at the top level.
	 * @param {Set<String>} [qualifiers] - Declaration qualifiers.
	 */

	parseStructDeclaration(global, qualifiers = new Set()) {

		this.expect("struct");

		if(this.peek().type === TOKEN_IDENTIFIER) {

			const nameToken = this.advance();

			if(global) {

				nameToken.value = this.scoper.declareGlobal(nameToken.value, true);

			} else {

				this.declareLocalType(nameToken);

			}

		}

		this.expect("{");

		// Parse the members. Member names are never renamed.
		while(this.peek().value !== "}") {

			if(this.peek() === EOF) {

				throw new Error("GLSLScoper: unterminated structure declaration");

			}

			if(this.peek().value === "struct") {

				throw new Error("GLSLScoper: nested structure declarations are not supported");

			}

			this.parseQualifiers();

			const typeToken = this.advance();

			if(typeToken.type !== TOKEN_IDENTIFIER) {

				throw new Error(`GLSLScoper: expected a member type, found "${typeToken.value}"`);

			}

			// Member types may refer to other structures of this effect.
			this.renameReference(typeToken);

			for(;;) {

				const memberToken = this.advance();

				if(memberToken.type !== TOKEN_IDENTIFIER) {

					throw new Error(`GLSLScoper: expected a member name, found "${memberToken.value}"`);

				}

				if(this.peek().value === "[") {

					this.advance();

					if(this.peek().value !== "]") {

						this.scanExpression(new Set(["]"]));

					}

					this.expect("]");

				}

				if(this.peek().value === ",") {

					this.advance();
					continue;

				}

				if(this.peek().value === ";") {

					this.advance();
					break;

				}

				throw new Error(`GLSLScoper: unexpected token "${this.peek().value}" in structure declaration`);

			}

		}

		this.expect("}");

		if(this.peek().value === ";") {

			this.advance();
			return;

		}

		// Variables of the new structure type.
		const nameToken = this.advance();

		if(nameToken.type !== TOKEN_IDENTIFIER) {

			throw new Error(`GLSLScoper: expected a variable name, found "${nameToken.value}"`);

		}

		this.parseDeclaratorList(nameToken, (t) => {

			if(global) {

				this.declareGlobalVariable(t, qualifiers);

			} else {

				this.declareLocal(t);

			}

		});

	}

	/**
	 * Parses a function parameter list. The cursor must be positioned after the opening parenthesis and the
	 * parameter scope must have been pushed by the caller.
	 *
	 * @private
	 */

	parseParameters() {

		if(this.peek().value === ")") {

			this.advance();
			return;

		}

		for(;;) {

			this.parseQualifiers();

			const typeToken = this.advance();

			if(typeToken.type !== TOKEN_IDENTIFIER) {

				throw new Error(`GLSLScoper: expected a parameter type, found "${typeToken.value}"`);

			}

			this.renameReference(typeToken);

			// The parameter name is optional in declarations.
			if(this.peek().type === TOKEN_IDENTIFIER) {

				this.declareLocal(this.advance());

			}

			if(this.peek().value === "[") {

				this.advance();

				if(this.peek().value !== "]") {

					this.scanExpression(new Set(["]"]));

				}

				this.expect("]");

			}

			if(this.peek().value === ",") {

				this.advance();
				continue;

			}

			if(this.peek().value === ")") {

				this.advance();
				return;

			}

			throw new Error(`GLSLScoper: unexpected token "${this.peek().value}" in parameter list`);

		}

	}

	/**
	 * Parses a list of variable declarators, starting with an already consumed name token.
	 *
	 * Names are registered after their initializers have been processed: the scope of a variable starts
	 * after its initializer, so an initializer cannot refer to the variable it belongs to.
	 *
	 * @private
	 * @param {Object} firstNameToken - The name token of the first declarator.
	 * @param {Function} declare - A callback that registers a name token.
	 */

	parseDeclaratorList(firstNameToken, declare) {

		let nameToken = firstNameToken;

		for(;;) {

			if(this.peek().value === "[") {

				this.advance();

				if(this.peek().value !== "]") {

					this.scanExpression(new Set(["]"]));

				}

				this.expect("]");

			}

			if(this.peek().value === "=") {

				this.advance();
				this.scanExpression(new Set([",", ";"]));

			}

			declare(nameToken);

			const token = this.peek();

			if(token.value === ",") {

				this.advance();
				nameToken = this.advance();

				if(nameToken.type !== TOKEN_IDENTIFIER) {

					throw new Error(`GLSLScoper: expected a variable name, found "${nameToken.value}"`);

				}

				continue;

			}

			if(token.value === ";") {

				this.advance();
				return;

			}

			throw new Error(`GLSLScoper: unexpected token "${token.value}" in declaration`);

		}

	}

	/**
	 * Parses a compound statement and pushes a new local scope.
	 *
	 * @private
	 */

	parseCompoundStatement() {

		this.expect("{");
		this.pushScope();

		while(this.peek().value !== "}") {

			if(this.peek() === EOF) {

				throw new Error("GLSLScoper: unterminated block");

			}

			this.parseStatement();

		}

		this.expect("}");
		this.popScope();

	}

	/**
	 * Checks whether the tokens at the cursor start a variable declaration.
	 *
	 * @private
	 * @return {Boolean} Whether a declaration follows.
	 */

	isDeclarationAhead() {

		let ahead = 0;

		while(QUALIFIERS.has(this.peek(ahead).value)) {

			ahead++;

		}

		const token = this.peek(ahead);

		if(token.value === "struct") {

			return true;

		}

		if(token.type !== TOKEN_IDENTIFIER) {

			return false;

		}

		return BUILTIN_TYPES.has(token.value) || this.isTypeName(token.value);

	}

	/**
	 * Parses a local variable declaration.
	 *
	 * @private
	 */

	parseLocalDeclaration() {

		if(this.peek().value === "struct") {

			this.parseStructDeclaration(false);
			return;

		}

		this.parseQualifiers();

		const typeToken = this.advance();

		if(typeToken.type !== TOKEN_IDENTIFIER) {

			throw new Error(`GLSLScoper: expected a type, found "${typeToken.value}"`);

		}

		this.renameReference(typeToken);

		const nameToken = this.advance();

		if(nameToken.type !== TOKEN_IDENTIFIER) {

			throw new Error(`GLSLScoper: expected a variable name, found "${nameToken.value}"`);

		}

		this.parseDeclaratorList(nameToken, (t) => this.declareLocal(t));

	}

	/**
	 * Parses a single statement.
	 *
	 * @private
	 */

	parseStatement() {

		const token = this.peek();

		switch(token.value) {

			case "{":
				this.parseCompoundStatement();
				return;

			case ";":
				this.advance();
				return;

			case "if":
				this.advance();
				this.expect("(");
				this.scanExpression(new Set([")"]));
				this.expect(")");
				this.parseStatement();

				if(this.peek().value === "else") {

					this.advance();
					this.parseStatement();

				}

				return;

			case "for":
				this.advance();
				this.expect("(");
				this.pushScope();

				if(this.peek().value === ";") {

					this.advance();

				} else if(this.isDeclarationAhead()) {

					this.parseLocalDeclaration();

				} else {

					this.scanExpression(new Set([";"]));
					this.expect(";");

				}

				if(this.peek().value !== ";") {

					this.scanExpression(new Set([";"]));

				}

				this.expect(";");

				if(this.peek().value !== ")") {

					this.scanExpression(new Set([")"]));

				}

				this.expect(")");
				this.parseStatement();
				this.popScope();
				return;

			case "while":
				this.advance();
				this.expect("(");
				this.scanExpression(new Set([")"]));
				this.expect(")");
				this.parseStatement();
				return;

			case "do":
				this.advance();
				this.parseStatement();

				if(this.peek().value !== "while") {

					throw new Error(`GLSLScoper: expected "while", found "${this.peek().value}"`);

				}

				this.advance();
				this.expect("(");
				this.scanExpression(new Set([")"]));
				this.expect(")");
				this.expect(";");
				return;

			case "return":
				this.advance();

				if(this.peek().value !== ";") {

					this.scanExpression(new Set([";"]));

				}

				this.expect(";");
				return;

			case "break":
			case "continue":
			case "discard":
				this.advance();
				this.expect(";");
				return;

			default:

				if(this.isDeclarationAhead()) {

					this.parseLocalDeclaration();
					return;

				}

				this.scanExpression(new Set([";"]));
				this.expect(";");
				return;

		}

	}

	/**
	 * Scans an expression and renames all identifiers that refer to global symbols of the current effect.
	 *
	 * Identifiers that follow a member selector are skipped, as are identifiers that refer to local
	 * variables, parameters or loop variables. The scan stops at one of the given token values when it is
	 * encountered at nesting depth zero. The stop token is not consumed.
	 *
	 * @private
	 * @param {Set<String>} stopValues - The token values that terminate the expression.
	 */

	scanExpression(stopValues) {

		let depth = 0;
		let previous = this.prevToken;

		for(;;) {

			const token = this.peek();

			if(token === EOF) {

				throw new Error("GLSLScoper: unexpected end of input in expression");

			}

			if(depth === 0 && stopValues.has(token.value)) {

				return;

			}

			if(token.value === "(" || token.value === "[" || token.value === "{") {

				depth++;

			} else if(token.value === ")" || token.value === "]" || token.value === "}") {

				if(depth === 0) {

					throw new Error(`GLSLScoper: unbalanced "${token.value}" in expression`);

				}

				depth--;

			}

			const current = this.advance();

			if(current.type === TOKEN_IDENTIFIER && (previous === null || previous.value !== ".")) {

				this.renameReference(current);

			}

			previous = current;

		}

	}

}

/**
 * Renames the private top-level symbols of effect shaders with a unique prefix.
 *
 * One scoper instance handles the fragment and vertex shader of a single effect so that interface names
 * such as varyings and uniforms are renamed consistently across both stages. A fresh instance must be
 * created for every effect and every compilation.
 */

export class GLSLScoper {

	/**
	 * Constructs a new GLSL scoper.
	 *
	 * @param {String} prefix - The prefix for the renamed symbols, e.g. "e0".
	 * @param {Iterable<String>} [uniformNames] - The names of the effect's uniforms.
	 * @param {Iterable<String>} [defineNames] - The names of the effect's preprocessor macros.
	 */

	constructor(prefix, uniformNames = [], defineNames = []) {

		/**
		 * The prefix for renamed symbols.
		 *
		 * @type {String}
		 * @private
		 */

		this.prefix = prefix;

		/**
		 * The global symbol table. Maps original names to their renamed versions.
		 *
		 * @type {Map<String, Object>}
		 * @private
		 */

		this.globals = new Map();

		/**
		 * The renamed varying names.
		 *
		 * @type {Set<String>}
		 * @readonly
		 */

		this.varyings = new Set();

		// Uniforms and macros have no GLSL declaration site to discover, so they are registered upfront.
		for(const name of uniformNames) {

			this.declareGlobal(name, false);

		}

		for(const key of defineNames) {

			const match = DEFINE_KEY.exec(key);
			this.declareGlobal((match !== null) ? match[1] : key, false);

		}

	}

	/**
	 * Builds the renamed version of the given name.
	 *
	 * @private
	 * @param {String} name - A name.
	 * @return {String} The renamed name.
	 */

	rename(name) {

		return this.prefix + name.charAt(0).toUpperCase() + name.slice(1);

	}

	/**
	 * Registers a global symbol and returns its renamed version.
	 *
	 * @param {String} name - The original name.
	 * @param {Boolean} isType - Whether the symbol is a structure type.
	 * @return {String} The renamed symbol.
	 */

	declareGlobal(name, isType) {

		let entry = this.globals.get(name);

		if(entry === undefined) {

			entry = { renamed: this.rename(name), isType };
			this.globals.set(name, entry);

		}

		return entry.renamed;

	}

	/**
	 * Returns the renamed version of the given global symbol, or null if the symbol is not effect-private.
	 *
	 * @param {String} name - The original name.
	 * @return {String} The renamed symbol, or null.
	 */

	resolveGlobal(name) {

		const entry = this.globals.get(name);
		return (entry !== undefined) ? entry.renamed : null;

	}

	/**
	 * Renames the effect-private symbols of the given shader source.
	 *
	 * This method may be called for the fragment shader and the vertex shader of one effect. Both sources
	 * share the same symbol table so that shared interface names stay connected.
	 *
	 * @param {String} source - The shader source code.
	 * @return {String} The renamed source code.
	 */

	scope(source) {

		const tokens = tokenize(source);
		new Parser(this, tokens).parse();
		return tokens.map((token) => token.value).join("");

	}

	/**
	 * Renames the keys and replacement texts of the given preprocessor macros.
	 *
	 * Object-like and function-like macro names are prefixed. Identifiers in the replacement text that refer
	 * to global symbols of this effect are renamed as well, except for function-like macro parameters which
	 * shadow same-named globals inside the replacement text.
	 *
	 * @param {Map<String, String>} defines - The macro definitions.
	 * @return {Map<String, String>} The renamed macro definitions.
	 */

	scopeDefines(defines) {

		const result = new Map();

		for(const [key, value] of defines) {

			const match = DEFINE_KEY.exec(key);
			const base = (match !== null) ? match[1] : key;
			const renamed = this.resolveGlobal(base) ?? this.rename(base);
			const newKey = renamed + ((match !== null) ? key.slice(base.length) : "");

			let newValue = value;

			if(typeof value === "string") {

				const params = (match !== null) ? macroParameters(match[2]) : null;
				newValue = this.renameInCode(value, params);

			}

			result.set(newKey, newValue);

		}

		return result;

	}

	/**
	 * Renames global symbol references inside a preprocessor directive token.
	 *
	 * The name and parameters of a `define` directive keep their meaning; references in the replacement
	 * text follow the effect's globals, with macro parameters shadowing same-named globals.
	 *
	 * @param {Object} token - The directive token.
	 */

	processDirective(token) {

		if(token.processed === true) {

			// Directives may be scanned multiple times during lookahead.
			return;

		}

		token.processed = true;

		const text = token.value;
		const defineMatch = /^(\s*#\s*define\s+[A-Za-z_]\w*)(\([^)]*\))?/.exec(text);

		if(defineMatch !== null) {

			const head = defineMatch[0];
			const params = macroParameters(defineMatch[2]);
			token.value = head + this.renameInCode(text.slice(head.length), params);

		} else {

			token.value = this.renameInCode(text, null);

		}

	}

	/**
	 * Renames global symbol references inside an arbitrary code fragment.
	 *
	 * @private
	 * @param {String} text - The code fragment.
	 * @param {Set<String>} [shadowed] - Names that shadow global symbols inside the fragment.
	 * @return {String} The renamed code fragment.
	 */

	renameInCode(text, shadowed) {

		const tokens = tokenize(text, false);
		let previous = null;

		for(const token of tokens) {

			if(token.type === TOKEN_IDENTIFIER &&
				(previous === null || previous.value !== ".") &&
				(shadowed === null || !shadowed.has(token.value))) {

				const renamed = this.resolveGlobal(token.value);

				if(renamed !== null) {

					token.value = renamed;

				}

			}

			if(token.type === TOKEN_IDENTIFIER || token.type === TOKEN_NUMBER || token.type === TOKEN_PUNCTUATION) {

				previous = token;

			}

		}

		return tokens.map((token) => token.value).join("");

	}

}
