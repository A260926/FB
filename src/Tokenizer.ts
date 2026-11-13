/* eslint-disable unicorn/prefer-includes-over-repeated-comparisons -- Keep hot ASCII comparisons allocation free. */
/* eslint-disable unicorn/no-break-in-nested-loop -- Keep the tokenizer state dispatch inline in its character-processing loop. */

import {
    DecodingMode,
    EntityDecoder,
    htmlDecodeTree,
    xmlDecodeTree,
} from "entities/decode";

const enum CharCodes {
    Tab = 0x9, // "\t"
    NewLine = 0xa, // "\n"
    FormFeed = 0xc, // "\f"
    CarriageReturn = 0xd, // "\r"
    Space = 0x20, // " "
    ExclamationMark = 0x21, // "!"
    Number = 0x23, // "#"
    Amp = 0x26, // "&"
    SingleQuote = 0x27, // "'"
    DoubleQuote = 0x22, // '"'
    Dash = 0x2d, // "-"
    Slash = 0x2f, // "/"
    Zero = 0x30, // "0"
    Nine = 0x39, // "9"
    Semi = 0x3b, // ";"
    Lt = 0x3c, // "<"
    Eq = 0x3d, // "="
    Gt = 0x3e, // ">"
    Questionmark = 0x3f, // "?"
    UpperA = 0x41, // "A"
    LowerA = 0x61, // "a"
    UpperF = 0x46, // "F"
    LowerF = 0x66, // "f"
    UpperZ = 0x5a, // "Z"
    LowerZ = 0x7a, // "z"
    LowerX = 0x78, // "x"
    OpeningSquareBracket = 0x5b, // "["
}

/** All the states the tokenizer can be in. */
const enum State {
    Text = 1,
    BeforeTagName, // After <
    InTagName,
    InSelfClosingTag,
    BeforeClosingTagName,
    InClosingTagName,
    AfterClosingTagName,

    // Attributes
    BeforeAttributeName,
    InAttributeName,
    AfterAttributeName,
    BeforeAttributeValue,
    InAttributeValueDq, // "
    InAttributeValueSq, // '
    InAttributeValueNq,

    // Declarations
    BeforeDeclaration, // !
    InDeclaration,

    // Processing instructions
    InProcessingInstruction, // ?

    // Comments & CDATA
    BeforeComment,
    CDATASequence,
    DeclarationSequence,
    InSpecialComment,
    InCommentLike,

    // Special tags
    SpecialStartSequence,
    InSpecialTag,
    InPlainText,

    /*
     * Script data (`<script>` in HTML content). This is the family of states
     * described by the HTML tokenizer's "script data" section.
     */
    InScriptData,
    ScriptDataLessThan,
    ScriptDataEndTagOpen,
    ScriptDataEndTagName,
    ScriptDataEscapeStart,
    ScriptDataEscapeStartDash,
    ScriptDataEscaped,
    ScriptDataEscapedDash,
    ScriptDataEscapedDashDash,
    ScriptDataEscapedLessThan,
    ScriptDataEscapedEndTagOpen,
    ScriptDataEscapedEndTagName,
    ScriptDataDoubleEscapeStart,
    ScriptDataDoubleEscaped,
    ScriptDataDoubleEscapedDash,
    ScriptDataDoubleEscapedDashDash,
    ScriptDataDoubleEscapedLessThan,
    ScriptDataDoubleEscapeEnd,

    InEntity,
}

/** Name of the tag whose contents use the script-data states. */
const ScriptName = "script";

function isWhitespace(c: number): boolean {
    return (
        c === CharCodes.Space ||
        c === CharCodes.NewLine ||
        c === CharCodes.Tab ||
        c === CharCodes.FormFeed ||
        c === CharCodes.CarriageReturn
    );
}

function isEndOfTagSection(c: number): boolean {
    return c === CharCodes.Slash || c === CharCodes.Gt || isWhitespace(c);
}

function isASCIIAlpha(c: number): boolean {
    return (
        (c >= CharCodes.LowerA && c <= CharCodes.LowerZ) ||
        (c >= CharCodes.UpperA && c <= CharCodes.UpperZ)
    );
}

/**
 * Quote style used for parsed attributes.
 */
export enum QuoteType {
    NoValue = 0,
    Unquoted = 1,
    Single = 2,
    Double = 3,
}

/**
 * Low-level tokenizer callback interface.
 */
export interface Callbacks {
    onattribdata(start: number, endIndex: number): void;
    onattribentity(codepoint: number): void;
    onattribend(quote: QuoteType, endIndex: number): void;
    onattribname(start: number, endIndex: number): void;
    oncdata(start: number, endIndex: number, endOffset: number): void;
    onclosetag(start: number, endIndex: number): void;
    oncomment(start: number, endIndex: number, endOffset: number): void;
    ondeclaration(start: number, endIndex: number): void;
    onend(): void;
    onopentagend(endIndex: number): void;
    onopentagname(start: number, endIndex: number): void;
    onprocessinginstruction(start: number, endIndex: number): void;
    onselfclosingtag(endIndex: number): void;
    ontext(start: number, endIndex: number): void;
    ontextentity(codepoint: number, endIndex: number): void;
    isInForeignContext?(): boolean;
    isCurrentNodeForeign?(): boolean;
}

/**
 * Sequences used to match longer strings.
 *
 * We don't have `Script`, `Style`, or `Title` here. Instead, we re-use the *End
 * sequences with an increased offset.
 */
const Sequences = {
    Empty: new Uint8Array(0),
    Cdata: new Uint8Array([0x43, 0x44, 0x41, 0x54, 0x41, 0x5b]), // CDATA[
    CdataEnd: new Uint8Array([0x5d, 0x5d, 0x3e]), // ]]>
    CommentEnd: new Uint8Array([0x2d, 0x2d, 0x21, 0x3e]), // `--!>`
    Doctype: new Uint8Array([0x64, 0x6f, 0x63, 0x74, 0x79, 0x70, 0x65]), // `doctype`
    IframeEnd: new Uint8Array([0x3c, 0x2f, 0x69, 0x66, 0x72, 0x61, 0x6d, 0x65]), // `</iframe`
    NoembedEnd: new Uint8Array([
        0x3c, 0x2f, 0x6e, 0x6f, 0x65, 0x6d, 0x62, 0x65, 0x64,
    ]), // `</noembed`
    NoframesEnd: new Uint8Array([
        0x3c, 0x2f, 0x6e, 0x6f, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x73,
    ]), // `</noframes`
    Plaintext: new Uint8Array([
        0x3c, 0x2f, 0x70, 0x6c, 0x61, 0x69, 0x6e, 0x74, 0x65, 0x78, 0x74,
    ]), // `</plaintext`
    ScriptEnd: new Uint8Array([0x3c, 0x2f, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74]), // `</script`
    StyleEnd: new Uint8Array([0x3c, 0x2f, 0x73, 0x74, 0x79, 0x6c, 0x65]), // `</style`
    TitleEnd: new Uint8Array([0x3c, 0x2f, 0x74, 0x69, 0x74, 0x6c, 0x65]), // `</title`
    TextareaEnd: new Uint8Array([
        0x3c, 0x2f, 0x74, 0x65, 0x78, 0x74, 0x61, 0x72, 0x65, 0x61,
    ]), // `</textarea`
    XmpEnd: new Uint8Array([0x3c, 0x2f, 0x78, 0x6d, 0x70]), // `</xmp`
};

/**
 * Maps the first lowercase character of an HTML tag name to the sequence
 * used for special-tag detection.  All sequences share a common layout
 * where index 2 is the first tag-name character, so matching always
 * continues from offset 3.
 */
const specialStartSequences = new Map<number, Uint8Array>([
    [Sequences.IframeEnd[2], Sequences.IframeEnd],
    [Sequences.NoembedEnd[2], Sequences.NoembedEnd],
    [Sequences.Plaintext[2], Sequences.Plaintext],
    [Sequences.ScriptEnd[2], Sequences.ScriptEnd],
    [Sequences.TitleEnd[2], Sequences.TitleEnd],
    [Sequences.XmpEnd[2], Sequences.XmpEnd],
]);

/**
 * Tokenizer implementation used by `Parser`.
 */
export default class Tokenizer {
    /** The current state the tokenizer is in. */
    private state = State.Text;
    /** The read buffer. */
    private buffer = "";
    /** The beginning of the section that is currently being read. */
    private sectionStart = 0;
    /** The index within the buffer that we are currently looking at. */
    private index = 0;
    /** The start of the last entity. */
    private entityStart = 0;
    /** Some behavior, eg. when decoding entities, is done while we are in another state. This keeps track of the other state type. */
    private baseState = State.Text;
    /** For special parsing behavior inside of script and style tags. */
    private isSpecial = false;
    /** The offset of the current buffer. */
    private offset = 0;

    private readonly xmlMode: boolean;
    private readonly decodeEntities: boolean;
    private readonly recognizeSelfClosing: boolean;
    private readonly recognizeCDATA: boolean;
    private readonly entityDecoder: EntityDecoder;

    private currentSequence: Uint8Array = Sequences.Empty;
    private sequenceIndex = 0;
    /**
     * Absolute index of the `<` that started a potential `</script` end tag in
     * script data. `-1` when no such candidate is pending.
     *
     * This is an absolute stream position, so it stays valid when `write()`
     * replaces the input buffer.
     */
    private scriptEndTagStart = -1;
    /** Number of characters of the `script` end-tag name matched so far. */
    private scriptEndTagIndex = 0;
    /**
     * Temporary word collected in the script-data double-escape states.
     * Stored as a string so it survives `write()` buffer replacement.
     */
    private scriptTempBuffer = "";
    /** Indicates whether the end of the input has been handled. */
    private finished = false;
    /** Indicates whether the tokenizer has been paused. */
    running = true;

    constructor(
        {
            xmlMode = false,
            decodeEntities = true,
            recognizeSelfClosing = xmlMode,
            recognizeCDATA = xmlMode,
        }: {
            xmlMode?: boolean;
            decodeEntities?: boolean;
            recognizeSelfClosing?: boolean;
            recognizeCDATA?: boolean;
        },
        private readonly cbs: Callbacks,
    ) {
        this.xmlMode = xmlMode;
        this.decodeEntities = decodeEntities;
        this.recognizeSelfClosing = recognizeSelfClosing;
        this.recognizeCDATA = xmlMode || recognizeCDATA;
        this.entityDecoder = new EntityDecoder(
            xmlMode ? xmlDecodeTree : htmlDecodeTree,
            (cp, consumed) => this.emitCodePoint(cp, consumed),
        );
    }

    private stateText(c: number): void {
        if (
            c === CharCodes.Lt ||
            (!this.decodeEntities && this.fastForwardTo(CharCodes.Lt))
        ) {
            if (this.index > this.sectionStart) {
                this.cbs.ontext(this.sectionStart, this.index);
            }
            this.state = State.BeforeTagName;
            this.sectionStart = this.index;
        } else if (this.decodeEntities && c === CharCodes.Amp) {
            this.startEntity();
        }
    }

    private enterTagBody(): void {
        if (this.currentSequence === Sequences.Plaintext) {
            this.currentSequence = Sequences.Empty;
            this.state = State.InPlainText;
        } else if (
            this.currentSequence === Sequences.ScriptEnd &&
            !this.xmlMode &&
            !this.cbs.isInForeignContext?.()
        ) {
            /*
             * `<script>` in HTML content uses the script-data state family.
             * XML mode and foreign content keep the generic raw-text handling.
             */
            this.state = State.InScriptData;
            this.scriptEndTagStart = -1;
            this.scriptEndTagIndex = 0;
            this.scriptTempBuffer = "";
        } else if (this.isSpecial) {
            this.state = State.InSpecialTag;
            this.sequenceIndex = 0;
        } else {
            this.state = State.Text;
        }
    }

    /**
     * Match the opening tag name against an HTML text-only tag sequence.
     *
     * Some tags share an initial prefix (`script`/`style`, `title`/`textarea`,
     * `noembed`/`noframes`), so we may switch to an alternate sequence at the
     * first distinguishing byte.  On a successful full match we fall back to
     * the normal tag-name state; a later `>` will enter raw-text, RCDATA, or
     * plaintext mode based on `currentSequence` / `isSpecial`.
     * @param c Current character code point.
     */
    private stateSpecialStartSequence(c: number): void {
        const lower = c | 0x20;

        // Still matching — check for an alternate sequence at branch points.
        if (this.sequenceIndex < this.currentSequence.length) {
            if (lower === this.currentSequence[this.sequenceIndex]) {
                this.sequenceIndex++;
                return;
            }

            if (this.sequenceIndex === 3) {
                if (
                    this.currentSequence === Sequences.ScriptEnd &&
                    lower === Sequences.StyleEnd[3]
                ) {
                    this.currentSequence = Sequences.StyleEnd;
                    this.sequenceIndex = 4;
                    return;
                }

                if (
                    this.currentSequence === Sequences.TitleEnd &&
                    lower === Sequences.TextareaEnd[3]
                ) {
                    this.currentSequence = Sequences.TextareaEnd;
                    this.sequenceIndex = 4;
                    return;
                }
            } else if (
                this.sequenceIndex === 4 &&
                this.currentSequence === Sequences.NoembedEnd &&
                lower === Sequences.NoframesEnd[4]
            ) {
                this.currentSequence = Sequences.NoframesEnd;
                this.sequenceIndex = 5;
                return;
            }
        } else if (isEndOfTagSection(c)) {
            // Full match on a valid tag boundary — keep the sequence.
            this.sequenceIndex = 0;
            this.state = State.InTagName;
            this.stateInTagName(c);
            return;
        }

        // No match — abandon special-tag detection.
        this.isSpecial = false;
        this.currentSequence = Sequences.Empty;
        this.sequenceIndex = 0;
        this.state = State.InTagName;
        this.stateInTagName(c);
    }

    private stateCDATASequence(c: number): void {
        if (c === Sequences.Cdata[this.sequenceIndex]) {
            if (++this.sequenceIndex === Sequences.Cdata.length) {
                this.sequenceIndex = 0;
                if (
                    this.recognizeCDATA ||
                    (this.cbs.isCurrentNodeForeign?.() ??
                        this.cbs.isInForeignContext?.())
                ) {
                    this.state = State.InCommentLike;
                    this.currentSequence = Sequences.CdataEnd;
                    this.sectionStart = this.index + 1;
                } else {
                    // In HTML, `<![CDATA[` is a bogus comment that ends at the first `>`.
                    this.state = State.InSpecialComment;
                }
            }
        } else {
            this.sequenceIndex = 0;
            if (this.xmlMode) {
                this.state = State.InDeclaration;
                this.stateInDeclaration(c); // Reconsume the character
            } else {
                this.state = State.InSpecialComment;
                this.stateInSpecialComment(c); // Reconsume the character
            }
        }
    }

    /**
     * When we wait for one specific character, we can speed things up
     * by skipping through the buffer until we find it.
     * @param c Current character code point.
     * @returns Whether the character was found.
     */
    private fastForwardTo(c: number): boolean {
        while (++this.index < this.buffer.length + this.offset) {
            if (this.buffer.charCodeAt(this.index - this.offset) === c) {
                return true;
            }
        }

        /*
         * We increment the index at the end of the `parse` loop,
         * so set it to `buffer.length - 1` here.
         *
         * TODO: Refactor `parse` to increment index before calling states.
         */
        this.index = this.buffer.length + this.offset - 1;

        return false;
    }

    /**
     * Emit a comment token and return to the text state.
     * @param offset Number of characters in the end sequence that have already been matched.
     */
    private emitComment(offset: number): void {
        this.cbs.oncomment(this.sectionStart, this.index, offset);
        this.sequenceIndex = 0;
        this.sectionStart = this.index + 1;
        this.state = State.Text;
    }

    /**
     * Comments and CDATA end with `-->` and `]]>`.
     *
     * Their common qualities are:
     * - Their end sequences have a distinct character they start with.
     * - That character is then repeated, so we have to check multiple repeats.
     * - All characters but the start character of the sequence can be skipped.
     * @param c Current character code point.
     */
    private stateInCommentLike(c: number): void {
        if (
            !this.xmlMode &&
            this.currentSequence === Sequences.CommentEnd &&
            this.sequenceIndex <= 1 &&
            /*
             * We're still at the very start of the comment: the only
             * characters consumed since `<!--` are the dashes that
             * advanced sequenceIndex (0 for `<!-->`, 1 for `<!--->`).
             */
            this.index === this.sectionStart + this.sequenceIndex &&
            c === CharCodes.Gt
        ) {
            // Abruptly closed empty HTML comment.
            this.emitComment(this.sequenceIndex);
        } else if (
            this.currentSequence === Sequences.CommentEnd &&
            this.sequenceIndex === 2 &&
            c === CharCodes.Gt
        ) {
            // `!` is optional here, so the same sequence also accepts `-->`.
            this.emitComment(2);
        } else if (
            this.currentSequence === Sequences.CommentEnd &&
            this.sequenceIndex === this.currentSequence.length - 1 &&
            c !== CharCodes.Gt
        ) {
            this.sequenceIndex = Number(c === CharCodes.Dash);
        } else if (c === this.currentSequence[this.sequenceIndex]) {
            if (++this.sequenceIndex === this.currentSequence.length) {
                if (this.currentSequence === Sequences.CdataEnd) {
                    this.cbs.oncdata(this.sectionStart, this.index, 2);
                } else {
                    this.cbs.oncomment(this.sectionStart, this.index, 3);
                }

                this.sequenceIndex = 0;
                this.sectionStart = this.index + 1;
                this.state = State.Text;
            }
        } else if (this.sequenceIndex === 0) {
            // Fast-forward to the first character of the sequence
            if (this.fastForwardTo(this.currentSequence[0])) {
                this.sequenceIndex = 1;
            }
        } else if (c !== this.currentSequence[this.sequenceIndex - 1]) {
            // Allow long sequences, eg. --->, ]]]>
            this.sequenceIndex = 0;
        }
    }

    /**
     * HTML only allows ASCII alpha characters (a-z and A-Z) at the beginning of a tag name.
     *
     * XML allows a lot more characters here (@see https://www.w3.org/TR/REC-xml/#NT-NameStartChar).
     * We allow anything that wouldn't end the tag.
     * @param c Current character code point.
     */
    private isTagStartChar(c: number) {
        return this.xmlMode ? !isEndOfTagSection(c) : isASCIIAlpha(c);
    }

    /**
     * Scan raw-text / RCDATA content for the matching end tag.
     *
     * For RCDATA tags (`<title>`, `<textarea>`) entities are decoded inline.
     * For raw-text tags (`<script>`, `<style>`, etc.) we fast-forward to `<`.
     * @param c Current character code point.
     */
    private stateInSpecialTag(c: number): void {
        if (this.sequenceIndex === this.currentSequence.length) {
            if (isEndOfTagSection(c)) {
                const endOfText = this.index - this.currentSequence.length;

                if (this.sectionStart < endOfText) {
                    // Spoof the index so that reported locations match up.
                    const actualIndex = this.index;
                    this.index = endOfText;
                    this.cbs.ontext(this.sectionStart, endOfText);
                    this.index = actualIndex;
                }

                this.isSpecial = false;
                this.sectionStart = endOfText + 2; // Skip over the `</`
                this.stateInClosingTagName(c);
                return; // We are done; skip the rest of the function.
            }

            this.sequenceIndex = 0;
        }

        if ((c | 0x20) === this.currentSequence[this.sequenceIndex]) {
            this.sequenceIndex += 1;
        } else if (this.sequenceIndex === 0) {
            if (
                this.currentSequence === Sequences.TitleEnd ||
                this.currentSequence === Sequences.TextareaEnd
            ) {
                // RCDATA tags have to parse entities while still looking for their end tag.
                if (this.decodeEntities && c === CharCodes.Amp) {
                    this.startEntity();
                }
            } else if (this.fastForwardTo(CharCodes.Lt)) {
                // Outside of RCDATA tags, we can fast-forward.
                this.sequenceIndex = 1;
            }
        } else {
            // If we see a `<`, set the sequence index to 1; useful for eg. `<</script>`.
            this.sequenceIndex = Number(c === CharCodes.Lt);
        }
    }

    /**
     * Absolute index up to which pending script text can be emitted without
     * cutting through a potential end tag.
     *
     * Script text is emitted lazily from `sectionStart`. While a `</script`
     * candidate is being matched (or a lone `<` is awaited) part of the
     * pending text may still turn into markup, so it must not be reported yet.
     * Returning `-1` signals that the current state is not a script-data one.
     * @returns Exclusive end of the safely-emittable text.
     */
    private scriptTextSafeEnd(): number {
        switch (this.state) {
            case State.ScriptDataLessThan:
            case State.ScriptDataEscapedLessThan: {
                // The `<` that may begin an end tag is at the previous index.
                return this.index - 1;
            }
            case State.ScriptDataEndTagOpen:
            case State.ScriptDataEndTagName:
            case State.ScriptDataEscapedEndTagOpen:
            case State.ScriptDataEscapedEndTagName: {
                return this.scriptEndTagStart >= 0
                    ? this.scriptEndTagStart
                    : this.index;
            }
            case State.InScriptData:
            case State.ScriptDataEscapeStart:
            case State.ScriptDataEscapeStartDash:
            case State.ScriptDataEscaped:
            case State.ScriptDataEscapedDash:
            case State.ScriptDataEscapedDashDash:
            case State.ScriptDataDoubleEscapeStart:
            case State.ScriptDataDoubleEscaped:
            case State.ScriptDataDoubleEscapedDash:
            case State.ScriptDataDoubleEscapedDashDash:
            case State.ScriptDataDoubleEscapedLessThan:
            case State.ScriptDataDoubleEscapeEnd: {
                return this.index;
            }
            default: {
                return -1;
            }
        }
    }

    /**
     * Emit the pending script text up to `endIndex`, if any.
     * @param endIndex Exclusive end of the text to emit.
     */
    private emitScriptText(endIndex: number): void {
        if (this.sectionStart < endIndex) {
            this.cbs.ontext(this.sectionStart, endIndex);
        }
    }

    /**
     * Handle the character that confirms a real `</script` end tag:
     * emit the remaining raw text, then treat the name like any closing tag.
     * @param c The delimiter that follows the end-tag name.
     */
    private closeScriptTag(c: number): void {
        const endOfText = this.scriptEndTagStart;
        this.emitScriptText(endOfText);

        this.scriptEndTagStart = -1;
        this.scriptEndTagIndex = 0;
        this.scriptTempBuffer = "";
        this.isSpecial = false;
        this.currentSequence = Sequences.Empty;
        this.sequenceIndex = 0;

        // Skip over `</`; the name starts here.
        this.sectionStart = endOfText + 2;
        this.stateInClosingTagName(c);
    }

    /**
     * Append an ASCII letter to the temporary word used by the double-escape
     * states. The buffer is capped: anything longer than `script` can never
     * match, and capping keeps memory bounded on hostile input.
     * @param c Current character code point.
     */
    private pushScriptTempBuffer(c: number): void {
        if (this.scriptTempBuffer.length <= ScriptName.length) {
            this.scriptTempBuffer += String.fromCharCode(c | 0x20);
        }
    }

    /**
     * Whether the collected temporary word is the script tag name.
     * @returns `true` for a case-insensitive `script`.
     */
    private isScriptTempBuffer(): boolean {
        return this.scriptTempBuffer === ScriptName;
    }

    private stateInScriptData(c: number): void {
        if (c !== CharCodes.Lt && !this.fastForwardTo(CharCodes.Lt)) {
            return;
        }

        this.scriptEndTagStart = this.index;
        this.state = State.ScriptDataLessThan;
    }

    private stateScriptDataLessThan(c: number): void {
        if (c === CharCodes.Slash) {
            this.state = State.ScriptDataEndTagOpen;
        } else if (c === CharCodes.ExclamationMark) {
            this.scriptEndTagStart = -1;
            this.state = State.ScriptDataEscapeStart;
        } else {
            this.scriptEndTagStart = -1;
            this.state = State.InScriptData;
            this.stateInScriptData(c);
        }
    }

    private stateScriptDataEndTagOpen(c: number): void {
        if (isASCIIAlpha(c)) {
            this.scriptEndTagIndex = 0;
            this.state = State.ScriptDataEndTagName;
            this.stateScriptDataEndTagName(c);
        } else {
            this.scriptEndTagStart = -1;
            this.state = State.InScriptData;
            this.stateInScriptData(c);
        }
    }

    /**
     * Match the name of a potential `</script` end tag.
     *
     * Only the exact name `script` (ASCII case-insensitive) followed by ASCII
     * whitespace, `/` or `>` closes the tag. Anything else is plain text.
     * @param c Current character code point.
     * @param isEscaped Whether the tag is being matched in escaped script data.
     */
    private handleScriptEndTagName(c: number, isEscaped: boolean): void {
        if (isASCIIAlpha(c)) {
            if (this.scriptEndTagIndex >= 0) {
                if (
                    this.scriptEndTagIndex < ScriptName.length &&
                    (c | 0x20) === ScriptName.charCodeAt(this.scriptEndTagIndex)
                ) {
                    this.scriptEndTagIndex++;
                } else {
                    this.scriptEndTagIndex = -1;
                }
            }
            return;
        }

        if (
            isEndOfTagSection(c) &&
            this.scriptEndTagIndex === ScriptName.length
        ) {
            this.closeScriptTag(c);
            return;
        }

        // Not an appropriate end tag: the pending characters are just text.
        this.scriptEndTagStart = -1;
        this.scriptEndTagIndex = 0;
        this.state = isEscaped ? State.ScriptDataEscaped : State.InScriptData;
        if (isEscaped) {
            this.stateScriptDataEscaped(c);
        } else {
            this.stateInScriptData(c);
        }
    }

    private stateScriptDataEndTagName(c: number): void {
        this.handleScriptEndTagName(c, false);
    }

    private stateScriptDataEscapeStart(c: number): void {
        if (c === CharCodes.Dash) {
            this.state = State.ScriptDataEscapeStartDash;
        } else {
            this.state = State.InScriptData;
            this.stateInScriptData(c);
        }
    }

    private stateScriptDataEscapeStartDash(c: number): void {
        if (c === CharCodes.Dash) {
            this.state = State.ScriptDataEscapedDashDash;
        } else {
            this.state = State.InScriptData;
            this.stateInScriptData(c);
        }
    }

    private stateScriptDataEscaped(c: number): void {
        if (c === CharCodes.Dash) {
            this.state = State.ScriptDataEscapedDash;
        } else if (c === CharCodes.Lt) {
            this.state = State.ScriptDataEscapedLessThan;
        }
    }

    private stateScriptDataEscapedDash(c: number): void {
        switch (c) {
            case CharCodes.Dash: {
                this.state = State.ScriptDataEscapedDashDash;
                break;
            }
            case CharCodes.Lt: {
                this.state = State.ScriptDataEscapedLessThan;
                break;
            }
            case CharCodes.Gt: {
                this.state = State.InScriptData;
                break;
            }
            default: {
                this.state = State.ScriptDataEscaped;
            }
        }
    }

    private stateScriptDataEscapedDashDash(c: number): void {
        switch (c) {
            case CharCodes.Dash: {
                // Stay in the dash-dash state.
                break;
            }
            case CharCodes.Lt: {
                this.state = State.ScriptDataEscapedLessThan;
                break;
            }
            case CharCodes.Gt: {
                this.state = State.InScriptData;
                break;
            }
            default: {
                this.state = State.ScriptDataEscaped;
            }
        }
    }

    private stateScriptDataEscapedLessThan(c: number): void {
        if (c === CharCodes.Slash) {
            this.scriptEndTagStart = this.index - 1;
            this.state = State.ScriptDataEscapedEndTagOpen;
        } else if (isASCIIAlpha(c)) {
            this.scriptTempBuffer = "";
            this.state = State.ScriptDataDoubleEscapeStart;
            this.stateScriptDataDoubleEscapeStart(c);
        } else {
            this.state = State.ScriptDataEscaped;
            this.stateScriptDataEscaped(c);
        }
    }

    private stateScriptDataEscapedEndTagOpen(c: number): void {
        if (isASCIIAlpha(c)) {
            this.scriptEndTagIndex = 0;
            this.state = State.ScriptDataEscapedEndTagName;
            this.stateScriptDataEscapedEndTagName(c);
        } else {
            this.scriptEndTagStart = -1;
            this.state = State.ScriptDataEscaped;
            this.stateScriptDataEscaped(c);
        }
    }

    private stateScriptDataEscapedEndTagName(c: number): void {
        this.handleScriptEndTagName(c, true);
    }

    private stateScriptDataDoubleEscapeStart(c: number): void {
        if (isEndOfTagSection(c)) {
            this.state = this.isScriptTempBuffer()
                ? State.ScriptDataDoubleEscaped
                : State.ScriptDataEscaped;
            this.scriptTempBuffer = "";
        } else if (isASCIIAlpha(c)) {
            this.pushScriptTempBuffer(c);
        } else {
            this.scriptTempBuffer = "";
            this.state = State.ScriptDataEscaped;
            this.stateScriptDataEscaped(c);
        }
    }

    private stateScriptDataDoubleEscaped(c: number): void {
        if (c === CharCodes.Dash) {
            this.state = State.ScriptDataDoubleEscapedDash;
        } else if (c === CharCodes.Lt) {
            this.state = State.ScriptDataDoubleEscapedLessThan;
        }
    }

    private stateScriptDataDoubleEscapedDash(c: number): void {
        switch (c) {
            case CharCodes.Dash: {
                this.state = State.ScriptDataDoubleEscapedDashDash;
                break;
            }
            case CharCodes.Lt: {
                this.state = State.ScriptDataDoubleEscapedLessThan;
                break;
            }
            case CharCodes.Gt: {
                this.state = State.InScriptData;
                break;
            }
            default: {
                this.state = State.ScriptDataDoubleEscaped;
            }
        }
    }

    private stateScriptDataDoubleEscapedDashDash(c: number): void {
        switch (c) {
            case CharCodes.Dash: {
                // Stay in the dash-dash state.
                break;
            }
            case CharCodes.Lt: {
                this.state = State.ScriptDataDoubleEscapedLessThan;
                break;
            }
            case CharCodes.Gt: {
                this.state = State.InScriptData;
                break;
            }
            default: {
                this.state = State.ScriptDataDoubleEscaped;
            }
        }
    }

    private stateScriptDataDoubleEscapedLessThan(c: number): void {
        if (c === CharCodes.Slash) {
            this.scriptTempBuffer = "";
            this.state = State.ScriptDataDoubleEscapeEnd;
        } else {
            this.state = State.ScriptDataDoubleEscaped;
            this.stateScriptDataDoubleEscaped(c);
        }
    }

    private stateScriptDataDoubleEscapeEnd(c: number): void {
        if (isEndOfTagSection(c)) {
            this.state = this.isScriptTempBuffer()
                ? State.ScriptDataEscaped
                : State.ScriptDataDoubleEscaped;
            this.scriptTempBuffer = "";
        } else if (isASCIIAlpha(c)) {
            this.pushScriptTempBuffer(c);
        } else {
            this.scriptTempBuffer = "";
            this.state = State.ScriptDataDoubleEscaped;
            this.stateScriptDataDoubleEscaped(c);
        }
    }

    private stateBeforeTagName(c: number): void {
        if (c === CharCodes.ExclamationMark) {
            this.state = State.BeforeDeclaration;
            this.sectionStart = this.index + 1;
        } else if (c === CharCodes.Questionmark) {
            if (this.xmlMode) {
                this.state = State.InProcessingInstruction;
                this.sequenceIndex = 0;
                this.sectionStart = this.index + 1;
            } else {
                this.state = State.InSpecialComment;
                this.sectionStart = this.index;
            }
        } else if (this.isTagStartChar(c)) {
            this.sectionStart = this.index;

            const special =
                this.xmlMode || this.cbs.isInForeignContext?.()
                    ? undefined
                    : specialStartSequences.get(c | 0x20);

            if (special === undefined) {
                this.state = State.InTagName;
            } else {
                this.isSpecial = true;
                this.currentSequence = special;
                this.sequenceIndex = 3;
                this.state = State.SpecialStartSequence;
            }
        } else if (c === CharCodes.Slash) {
            this.state = State.BeforeClosingTagName;
        } else {
            this.state = State.Text;
            this.stateText(c);
        }
    }
    private stateInTagName(c: number): void {
        if (!isEndOfTagSection(c)) {
            return;
        }

        this.cbs.onopentagname(this.sectionStart, this.index);
        this.sectionStart = -1;
        this.state = State.BeforeAttributeName;
        this.stateBeforeAttributeName(c);
    }
    private stateBeforeClosingTagName(c: number): void {
        if (isWhitespace(c)) {
            if (this.xmlMode) {
                // Ignore
            } else {
                this.state = State.InSpecialComment;
                this.sectionStart = this.index;
            }
        } else if (c === CharCodes.Gt) {
            this.state = State.Text;
            if (!this.xmlMode) {
                this.sectionStart = this.index + 1;
            }
        } else {
            this.state = this.isTagStartChar(c)
                ? State.InClosingTagName
                : State.InSpecialComment;
            this.sectionStart = this.index;
        }
    }
    private stateInClosingTagName(c: number): void {
        if (!isEndOfTagSection(c)) {
            return;
        }

        this.cbs.onclosetag(this.sectionStart, this.index);
        this.sectionStart = -1;
        this.state = State.AfterClosingTagName;
        this.stateAfterClosingTagName(c);
    }
    private stateAfterClosingTagName(c: number): void {
        // Skip everything until ">"
        if (!(c === CharCodes.Gt || this.fastForwardTo(CharCodes.Gt))) {
            return;
        }

        this.state = State.Text;
        this.sectionStart = this.index + 1;
    }
    private stateBeforeAttributeName(c: number): void {
        if (c === CharCodes.Gt) {
            this.cbs.onopentagend(this.index);
            this.enterTagBody();
            this.sectionStart = this.index + 1;
        } else if (c === CharCodes.Slash) {
            this.state = State.InSelfClosingTag;
        } else if (!isWhitespace(c)) {
            this.state = State.InAttributeName;
            this.sectionStart = this.index;
        }
    }
    /**
     * Handle `/` before `>` in an opening tag.
     *
     * In HTML mode, text-only tags ignore the self-closing flag and still enter
     * their raw-text/RCDATA/plaintext state unless self-closing tags are being
     * recognized. In XML mode, or for ordinary tags, the tokenizer returns to
     * regular text parsing after emitting the self-closing callback.
     * @param c Current character code point.
     */
    private stateInSelfClosingTag(c: number): void {
        if (c === CharCodes.Gt) {
            this.cbs.onselfclosingtag(this.index);
            this.sectionStart = this.index + 1;

            if (!this.recognizeSelfClosing) {
                this.enterTagBody();
                return;
            }

            this.state = State.Text;
            this.isSpecial = false; // Reset special state, in case of self-closing special tags
            this.currentSequence = Sequences.Empty;
        } else if (!isWhitespace(c)) {
            this.state = State.BeforeAttributeName;
            this.stateBeforeAttributeName(c);
        }
    }
    private stateInAttributeName(c: number): void {
        if (!(c === CharCodes.Eq || isEndOfTagSection(c))) {
            return;
        }

        this.cbs.onattribname(this.sectionStart, this.index);
        this.sectionStart = this.index;
        this.state = State.AfterAttributeName;
        this.stateAfterAttributeName(c);
    }
    private stateAfterAttributeName(c: number): void {
        if (c === CharCodes.Eq) {
            this.state = State.BeforeAttributeValue;
        } else if (c === CharCodes.Slash || c === CharCodes.Gt) {
            this.cbs.onattribend(QuoteType.NoValue, this.sectionStart);
            this.sectionStart = -1;
            this.state = State.BeforeAttributeName;
            this.stateBeforeAttributeName(c);
        } else if (!isWhitespace(c)) {
            this.cbs.onattribend(QuoteType.NoValue, this.sectionStart);
            this.state = State.InAttributeName;
            this.sectionStart = this.index;
        }
    }
    private stateBeforeAttributeValue(c: number): void {
        if (c === CharCodes.DoubleQuote) {
            this.state = State.InAttributeValueDq;
            this.sectionStart = this.index + 1;
        } else if (c === CharCodes.SingleQuote) {
            this.state = State.InAttributeValueSq;
            this.sectionStart = this.index + 1;
        } else if (!isWhitespace(c)) {
            this.sectionStart = this.index;
            this.state = State.InAttributeValueNq;
            this.stateInAttributeValueNoQuotes(c); // Reconsume token
        }
    }
    private handleInAttributeValue(c: number, quote: number) {
        if (
            c === quote ||
            (!this.decodeEntities && this.fastForwardTo(quote))
        ) {
            this.cbs.onattribdata(this.sectionStart, this.index);
            this.sectionStart = -1;
            this.cbs.onattribend(
                quote === CharCodes.DoubleQuote
                    ? QuoteType.Double
                    : QuoteType.Single,
                this.index + 1,
            );
            this.state = State.BeforeAttributeName;
        } else if (this.decodeEntities && c === CharCodes.Amp) {
            this.startEntity();
        }
    }
    private stateInAttributeValueDoubleQuotes(c: number): void {
        this.handleInAttributeValue(c, CharCodes.DoubleQuote);
    }
    private stateInAttributeValueSingleQuotes(c: number): void {
        this.handleInAttributeValue(c, CharCodes.SingleQuote);
    }
    private stateInAttributeValueNoQuotes(c: number): void {
        if (isWhitespace(c) || c === CharCodes.Gt) {
            this.cbs.onattribdata(this.sectionStart, this.index);
            this.sectionStart = -1;
            this.cbs.onattribend(QuoteType.Unquoted, this.index);
            this.state = State.BeforeAttributeName;
            this.stateBeforeAttributeName(c);
        } else if (this.decodeEntities && c === CharCodes.Amp) {
            this.startEntity();
        }
    }
    /**
     * Distinguish between CDATA, declarations, HTML comments, and HTML bogus
     * comments after `<!`.
     *
     * In HTML mode, only real comments and doctypes stay on declaration paths;
     * everything else becomes a bogus comment terminated by the next `>`.
     * @param c Current character code point.
     */
    private stateBeforeDeclaration(c: number): void {
        if (c === CharCodes.OpeningSquareBracket) {
            this.state = State.CDATASequence;
            this.sequenceIndex = 0;
        } else if (this.xmlMode) {
            this.state =
                c === CharCodes.Dash
                    ? State.BeforeComment
                    : State.InDeclaration;
        } else if ((c | 0x20) === Sequences.Doctype[0]) {
            this.state = State.DeclarationSequence;
            this.currentSequence = Sequences.Doctype;
            this.sequenceIndex = 1;
        } else if (c === CharCodes.Gt) {
            this.cbs.oncomment(this.sectionStart, this.index, 0);
            this.state = State.Text;
            this.sectionStart = this.index + 1;
        } else if (c === CharCodes.Dash) {
            this.state = State.BeforeComment;
        } else {
            this.state = State.InSpecialComment;
        }
    }
    /**
     * Continue matching `doctype` after `<!d`.
     *
     * A full `doctype` match stays on the declaration path; any other name falls
     * back to an HTML bogus comment, which matches browser behavior for
     * non-doctype `<!...>` constructs.
     * @param c Current character code point.
     */
    private stateDeclarationSequence(c: number): void {
        if (this.sequenceIndex === this.currentSequence.length) {
            this.state = State.InDeclaration;
            this.stateInDeclaration(c);
        } else if ((c | 0x20) === this.currentSequence[this.sequenceIndex]) {
            this.sequenceIndex += 1;
        } else if (c === CharCodes.Gt) {
            this.cbs.oncomment(this.sectionStart, this.index, 0);
            this.state = State.Text;
            this.sectionStart = this.index + 1;
        } else {
            this.state = State.InSpecialComment;
        }
    }
    private stateInDeclaration(c: number): void {
        if (!(c === CharCodes.Gt || this.fastForwardTo(CharCodes.Gt))) {
            return;
        }

        this.cbs.ondeclaration(this.sectionStart, this.index);
        this.state = State.Text;
        this.sectionStart = this.index + 1;
    }
    /**
     * XML processing instructions (`<?...?>`).
     *
     * In HTML mode `<?` is routed to `InSpecialComment` instead, so this
     * state is only reachable in XML mode.
     * @param c Current character code point.
     */
    private stateInProcessingInstruction(c: number): void {
        if (c === CharCodes.Questionmark) {
            // Remember that we just consumed `?`, so the next `>` closes the PI.
            this.sequenceIndex = 1;
        } else if (c === CharCodes.Gt && this.sequenceIndex === 1) {
            this.cbs.onprocessinginstruction(this.sectionStart, this.index - 1);
            this.sequenceIndex = 0;
            this.state = State.Text;
            this.sectionStart = this.index + 1;
        } else {
            // Keep scanning for the next `?`, which can start a closing `?>`.
            this.sequenceIndex = Number(
                this.fastForwardTo(CharCodes.Questionmark),
            );
        }
    }
    private stateBeforeComment(c: number): void {
        if (c === CharCodes.Dash) {
            this.state = State.InCommentLike;
            this.currentSequence = Sequences.CommentEnd;
            this.sequenceIndex = 0;
            this.sectionStart = this.index + 1;
        } else if (this.xmlMode) {
            this.state = State.InDeclaration;
        } else if (c === CharCodes.Gt) {
            this.cbs.oncomment(this.sectionStart, this.index, 0);
            this.state = State.Text;
            this.sectionStart = this.index + 1;
        } else {
            this.state = State.InSpecialComment;
        }
    }
    private stateInSpecialComment(c: number): void {
        if (!(c === CharCodes.Gt || this.fastForwardTo(CharCodes.Gt))) {
            return;
        }

        this.cbs.oncomment(this.sectionStart, this.index, 0);
        this.state = State.Text;
        this.sectionStart = this.index + 1;
    }

    private startEntity() {
        this.baseState = this.state;
        this.state = State.InEntity;
        this.entityStart = this.index;
        this.entityDecoder.startEntity(
            this.xmlMode
                ? DecodingMode.Strict
                : this.baseState === State.Text ||
                    this.baseState === State.InSpecialTag
                  ? DecodingMode.Legacy
                  : DecodingMode.Attribute,
        );
    }

    private stateInEntity(): void {
        const indexInBuffer = this.index - this.offset;
        const length = this.entityDecoder.write(this.buffer, indexInBuffer);

        // If `length` is positive, we are done with the entity.
        if (length >= 0) {
            this.state = this.baseState;

            if (length === 0) {
                this.index -= 1;
            }
        } else {
            if (
                indexInBuffer < this.buffer.length &&
                this.buffer.charCodeAt(indexInBuffer) === CharCodes.Amp
            ) {
                this.state = this.baseState;
                this.index -= 1;
                return;
            }

            // Mark buffer as consumed.
            this.index = this.offset + this.buffer.length - 1;
        }
    }

    /**
     * Remove data that has already been consumed from the buffer.
     */
    private cleanup() {
        // If we are inside of text or attributes, emit what we already have.
        if (this.running && this.sectionStart !== this.index) {
            if (
                this.state === State.Text ||
                this.state === State.InPlainText ||
                (this.state === State.InSpecialTag && this.sequenceIndex === 0)
            ) {
                this.cbs.ontext(this.sectionStart, this.index);
                this.sectionStart = this.index;
            } else if (
                this.state === State.InAttributeValueDq ||
                this.state === State.InAttributeValueSq ||
                this.state === State.InAttributeValueNq
            ) {
                this.cbs.onattribdata(this.sectionStart, this.index);
                this.sectionStart = this.index;
            } else {
                /*
                 * Script data can be handed out incrementally, but a pending
                 * `<` or `</script` candidate must stay in the buffer until it
                 * is either confirmed or rejected.
                 */
                const safeEnd = this.scriptTextSafeEnd();
                if (safeEnd > this.sectionStart) {
                    this.cbs.ontext(this.sectionStart, safeEnd);
                    this.sectionStart = safeEnd;
                }
            }
        }
    }

    private shouldContinue() {
        return this.index < this.buffer.length + this.offset && this.running;
    }

    /**
     * Iterates through the buffer, calling the function corresponding to the current state.
     *
     * States that are more likely to be hit are higher up, as a performance improvement.
     */
    private parse() {
        while (this.shouldContinue()) {
            const c = this.buffer.charCodeAt(this.index - this.offset);
            switch (this.state) {
                case State.Text: {
                    this.stateText(c);
                    break;
                }
                case State.InPlainText: {
                    // Skip to end of buffer; cleanup() emits the text.
                    this.index = this.buffer.length + this.offset - 1;
                    break;
                }
                case State.SpecialStartSequence: {
                    this.stateSpecialStartSequence(c);
                    break;
                }
                case State.InSpecialTag: {
                    this.stateInSpecialTag(c);
                    break;
                }
                case State.InScriptData: {
                    this.stateInScriptData(c);
                    break;
                }
                case State.ScriptDataLessThan: {
                    this.stateScriptDataLessThan(c);
                    break;
                }
                case State.ScriptDataEndTagOpen: {
                    this.stateScriptDataEndTagOpen(c);
                    break;
                }
                case State.ScriptDataEndTagName: {
                    this.stateScriptDataEndTagName(c);
                    break;
                }
                case State.ScriptDataEscapeStart: {
                    this.stateScriptDataEscapeStart(c);
                    break;
                }
                case State.ScriptDataEscapeStartDash: {
                    this.stateScriptDataEscapeStartDash(c);
                    break;
                }
                case State.ScriptDataEscaped: {
                    this.stateScriptDataEscaped(c);
                    break;
                }
                case State.ScriptDataEscapedDash: {
                    this.stateScriptDataEscapedDash(c);
                    break;
                }
                case State.ScriptDataEscapedDashDash: {
                    this.stateScriptDataEscapedDashDash(c);
                    break;
                }
                case State.ScriptDataEscapedLessThan: {
                    this.stateScriptDataEscapedLessThan(c);
                    break;
                }
                case State.ScriptDataEscapedEndTagOpen: {
                    this.stateScriptDataEscapedEndTagOpen(c);
                    break;
                }
                case State.ScriptDataEscapedEndTagName: {
                    this.stateScriptDataEscapedEndTagName(c);
                    break;
                }
                case State.ScriptDataDoubleEscapeStart: {
                    this.stateScriptDataDoubleEscapeStart(c);
                    break;
                }
                case State.ScriptDataDoubleEscaped: {
                    this.stateScriptDataDoubleEscaped(c);
                    break;
                }
                case State.ScriptDataDoubleEscapedDash: {
                    this.stateScriptDataDoubleEscapedDash(c);
                    break;
                }
                case State.ScriptDataDoubleEscapedDashDash: {
                    this.stateScriptDataDoubleEscapedDashDash(c);
                    break;
                }
                case State.ScriptDataDoubleEscapedLessThan: {
                    this.stateScriptDataDoubleEscapedLessThan(c);
                    break;
                }
                case State.ScriptDataDoubleEscapeEnd: {
                    this.stateScriptDataDoubleEscapeEnd(c);
                    break;
                }
                case State.CDATASequence: {
                    this.stateCDATASequence(c);
                    break;
                }
                case State.DeclarationSequence: {
                    this.stateDeclarationSequence(c);
                    break;
                }
                case State.InAttributeValueDq: {
                    this.stateInAttributeValueDoubleQuotes(c);
                    break;
                }
                case State.InAttributeName: {
                    this.stateInAttributeName(c);
                    break;
                }
                case State.InCommentLike: {
                    this.stateInCommentLike(c);
                    break;
                }
                case State.InSpecialComment: {
                    this.stateInSpecialComment(c);
                    break;
                }
                case State.BeforeAttributeName: {
                    this.stateBeforeAttributeName(c);
                    break;
                }
                case State.InTagName: {
                    this.stateInTagName(c);
                    break;
                }
                case State.InClosingTagName: {
                    this.stateInClosingTagName(c);
                    break;
                }
                case State.BeforeTagName: {
                    this.stateBeforeTagName(c);
                    break;
                }
                case State.AfterAttributeName: {
                    this.stateAfterAttributeName(c);
                    break;
                }
                case State.InAttributeValueSq: {
                    this.stateInAttributeValueSingleQuotes(c);
                    break;
                }
                case State.BeforeAttributeValue: {
                    this.stateBeforeAttributeValue(c);
                    break;
                }
                case State.BeforeClosingTagName: {
                    this.stateBeforeClosingTagName(c);
                    break;
                }
                case State.AfterClosingTagName: {
                    this.stateAfterClosingTagName(c);
                    break;
                }
                case State.InAttributeValueNq: {
                    this.stateInAttributeValueNoQuotes(c);
                    break;
                }
                case State.InSelfClosingTag: {
                    this.stateInSelfClosingTag(c);
                    break;
                }
                case State.InDeclaration: {
                    this.stateInDeclaration(c);
                    break;
                }
                case State.BeforeDeclaration: {
                    this.stateBeforeDeclaration(c);
                    break;
                }
                case State.BeforeComment: {
                    this.stateBeforeComment(c);
                    break;
                }
                case State.InProcessingInstruction: {
                    this.stateInProcessingInstruction(c);
                    break;
                }
                case State.InEntity: {
                    this.stateInEntity();
                    break;
                }
            }
            this.index++;
        }
        this.cleanup();
    }

    private finish() {
        this.finished = true;
        if (this.state === State.InEntity) {
            this.entityDecoder.end();
            this.state = this.baseState;
        }

        this.handleTrailingData();

        this.cbs.onend();
    }

    private handleTrailingCommentLikeData(endIndex: number): boolean {
        if (this.state !== State.InCommentLike) {
            return false;
        }

        if (this.currentSequence === Sequences.CdataEnd) {
            if (
                this.xmlMode ||
                (this.cbs.isCurrentNodeForeign?.() ??
                    this.cbs.isInForeignContext?.())
            ) {
                if (this.sectionStart < endIndex) {
                    this.cbs.oncdata(this.sectionStart, endIndex, 0);
                }
            } else {
                /* In HTML content, unclosed CDATA is a bogus comment. */
                const cdataStart =
                    this.sectionStart - Sequences.Cdata.length - 1;
                this.cbs.oncomment(cdataStart, endIndex, 0);
            }
        } else {
            const offset = this.xmlMode
                ? 0
                : Math.min(this.sequenceIndex, Sequences.CommentEnd.length - 1);
            this.cbs.oncomment(this.sectionStart, endIndex, offset);
        }

        return true;
    }

    private handleTrailingMarkupDeclaration(endIndex: number): boolean {
        if (this.xmlMode) {
            switch (this.state) {
                case State.InSpecialComment:
                case State.BeforeComment:
                case State.CDATASequence:
                case State.DeclarationSequence:
                case State.InDeclaration: {
                    this.cbs.ontext(this.sectionStart, endIndex);
                    return true;
                }
                default: {
                    return false;
                }
            }
        }

        switch (this.state) {
            case State.BeforeDeclaration:
            case State.InSpecialComment:
            case State.BeforeComment:
            case State.CDATASequence: {
                this.cbs.oncomment(this.sectionStart, endIndex, 0);
                return true;
            }
            case State.DeclarationSequence: {
                if (this.sequenceIndex !== Sequences.Doctype.length) {
                    this.cbs.oncomment(this.sectionStart, endIndex, 0);
                }
                return true;
            }
            case State.InDeclaration: {
                return true;
            }
            default: {
                return false;
            }
        }
    }

    private handleTrailingScriptData(): boolean {
        if (this.scriptTextSafeEnd() === -1) {
            return false;
        }

        /*
         * At EOF every pending character is script text — including a partial
         * `</script` that never got a valid delimiter. Deliver it exactly once.
         */
        this.emitScriptText(this.buffer.length + this.offset);
        return true;
    }

    /** Handle any trailing data. */
    private handleTrailingData() {
        const endIndex = this.buffer.length + this.offset;

        if (
            this.handleTrailingCommentLikeData(endIndex) ||
            this.handleTrailingMarkupDeclaration(endIndex) ||
            this.handleTrailingScriptData()
        ) {
            return;
        }

        // If there is no remaining data, we are done.
        if (this.sectionStart >= endIndex) {
            return;
        }

        switch (this.state) {
            case State.InTagName:
            case State.BeforeAttributeName:
            case State.BeforeAttributeValue:
            case State.AfterAttributeName:
            case State.InAttributeName:
            case State.InAttributeValueSq:
            case State.InAttributeValueDq:
            case State.InAttributeValueNq:
            case State.InClosingTagName: {
                /*
                 * If we are currently in an opening or closing tag, us not calling the
                 * respective callback signals that the tag should be ignored.
                 */
                break;
            }
            default: {
                this.cbs.ontext(this.sectionStart, endIndex);
            }
        }
    }

    private emitCodePoint(cp: number, consumed: number): void {
        if (
            this.baseState !== State.Text &&
            this.baseState !== State.InSpecialTag
        ) {
            if (this.sectionStart < this.entityStart) {
                this.cbs.onattribdata(this.sectionStart, this.entityStart);
            }
            this.sectionStart = this.entityStart + consumed;
            this.index = this.sectionStart - 1;

            this.cbs.onattribentity(cp);
        } else {
            if (this.sectionStart < this.entityStart) {
                this.cbs.ontext(this.sectionStart, this.entityStart);
            }
            this.sectionStart = this.entityStart + consumed;
            this.index = this.sectionStart - 1;

            this.cbs.ontextentity(cp, this.sectionStart);
        }
    }

    reset(): void {
        this.state = State.Text;
        this.buffer = "";
        this.sectionStart = 0;
        this.index = 0;
        this.baseState = State.Text;
        this.isSpecial = false;
        this.currentSequence = Sequences.Empty;
        this.sequenceIndex = 0;
        this.scriptEndTagStart = -1;
        this.scriptEndTagIndex = 0;
        this.scriptTempBuffer = "";
        this.running = true;
        this.finished = false;
        this.offset = 0;
    }

    write(chunk: string): void {
        this.offset += this.buffer.length;
        this.buffer = chunk;
        this.parse();
    }

    end(): void {
        if (this.running && !this.finished) this.finish();
    }

    pause(): void {
        this.running = false;
    }

    resume(): void {
        this.running = true;
        if (!this.finished && this.index < this.buffer.length + this.offset) {
            this.parse();
        }
    }
}
