declare namespace parse {
	/** A shell control operator. */
	export interface ControlOperator {
		op: '||' | '&&' | ';;&' | ';;' | ';&' | '|&' | '<(' | '>(' | '<<<' | '<<-' | '<<' | '>>' | '>&' | '>|' | '&>>' | '&>' | '<&' | '<>' | '&' | ';' | '(' | ')' | '|' | '<' | '>';
	}

	/** A glob pattern parsed from the shell command. */
	export interface GlobPattern {
		op: 'glob';
		pattern: string;
	}

	/** A shell comment. */
	export interface Comment {
		comment: string;
	}

	/** A parsed token returned by {@link parse}. */
	export type ParseEntry = string | ControlOperator | GlobPattern | Comment;

	/** Options for the {@link parse} function. */
	export interface ParseOptions {
		/** Custom escape character. Defaults to `\\`. */
		escape?: string;
		/** Field-splits an unquoted variable expansion, the way a shell does using `IFS`; quoted expansions are never split. `true` uses the default IFS (space, tab, newline); a string uses its characters as the IFS. An empty string, `false`, or omitting the option disables splitting. Defaults to false. */
		splitUnquoted?: boolean | string;
		/** Strict argv-only mode: accepts only the argument syntax of a single command and returns `string[]`. Cannot be combined with a non-default `escape` or an enabled `splitUnquoted`. Defaults to false. */
		argvOnly?: boolean;
	}

	export type Env =
		| Record<string, string | undefined>
		| ((key: string) => string | object | undefined);

	/** Environment for `argvOnly` mode: lookups may only yield strings or `undefined`. */
	export type ArgvEnv =
		| Record<string, string | undefined>
		| ((key: string) => string | undefined);
}

/**
 * Parses a shell command string into an array of tokens.
 *
 * @param s - The shell command string to parse.
 * @param env - Optional environment variables for expansion, either as an object of string values or a lookup function. When the lookup function returns an object, that object is inserted into the result verbatim.
 * @param opts - Optional parsing options.
 * @returns An array of parsed tokens, including any objects returned by an `env` lookup function.
 */
declare function parse<T extends string | object = never>(
	s: string,
	env?: parse.Env,
	opts?: parse.ParseOptions & { argvOnly?: false },
): (parse.ParseEntry | T)[];

/**
 * Parses the argument syntax of a single command into a plain `string[]`.
 *
 * The source is fully validated before `env` is consulted, so a rejected
 * source performs zero lookups. Operator objects, globs, and comments are
 * never produced: anything that would become one is a `SyntaxError`.
 *
 * @param s - The argument string to parse.
 * @param env - Optional environment variables; values must be strings or `undefined`, anything else throws a `TypeError`.
 * @param opts - Must set `argvOnly: true`; cannot enable `splitUnquoted` or set a non-default `escape`.
 * @returns The parsed arguments, ready to hand to a non-shell spawn API.
 */
declare function parse(
	s: string,
	env: parse.ArgvEnv | undefined,
	opts: parse.ParseOptions & { argvOnly: true },
): string[];

export = parse;
