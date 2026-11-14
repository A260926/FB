#include "svgtextelement.h"
#include "svglayoutstate.h"
#include "svgrenderstate.h"
#include "svggeometryelement.h"
#include "pathmeasure.h"

#include <cassert>
#include <cmath>

namespace lunasvg {

inline const SVGTextNode* toSVGTextNode(const SVGNode* node)
{
    assert(node && node->isTextNode());
    return static_cast<const SVGTextNode*>(node);
}

inline const SVGTextPositioningElement* toSVGTextPositioningElement(const SVGNode* node)
{
    assert(node && node->isTextPositioningElement());
    return static_cast<const SVGTextPositioningElement*>(node);
}

static AlignmentBaseline resolveDominantBaseline(const SVGTextPositioningElement* element)
{
    switch(element->dominant_baseline()) {
    case DominantBaseline::Auto:
        if(element->isVerticalWritingMode())
            return AlignmentBaseline::Central;
        return AlignmentBaseline::Alphabetic;
    case DominantBaseline::UseScript:
    case DominantBaseline::NoChange:
    case DominantBaseline::ResetSize:
        return AlignmentBaseline::Auto;
    case DominantBaseline::Ideographic:
        return AlignmentBaseline::Ideographic;
    case DominantBaseline::Alphabetic:
        return AlignmentBaseline::Alphabetic;
    case DominantBaseline::Hanging:
        return AlignmentBaseline::Hanging;
    case DominantBaseline::Mathematical:
        return AlignmentBaseline::Mathematical;
    case DominantBaseline::Central:
        return AlignmentBaseline::Central;
    case DominantBaseline::Middle:
        return AlignmentBaseline::Middle;
    case DominantBaseline::TextAfterEdge:
        return AlignmentBaseline::TextAfterEdge;
    case DominantBaseline::TextBeforeEdge:
        return AlignmentBaseline::TextBeforeEdge;
    default:
        assert(false);
    }

    return AlignmentBaseline::Auto;
}

static float calculateBaselineOffset(const SVGTextPositioningElement* element)
{
    auto offset = element->baseline_offset();
    auto parent = element->parentElement();
    while(parent->isTextPositioningElement()) {
        offset += toSVGTextPositioningElement(parent)->baseline_offset();
        parent = parent->parentElement();
    }

    auto baseline = element->alignment_baseline();
    if(baseline == AlignmentBaseline::Auto || baseline == AlignmentBaseline::Baseline) {
        baseline = resolveDominantBaseline(element);
    }

    const auto& font = element->font();
    switch(baseline) {
    case AlignmentBaseline::BeforeEdge:
    case AlignmentBaseline::TextBeforeEdge:
        offset -= font.ascent();
        break;
    case AlignmentBaseline::Middle:
        offset -= font.xHeight() / 2.f;
        break;
    case AlignmentBaseline::Central:
        offset -= (font.ascent() + font.descent()) / 2.f;
        break;
    case AlignmentBaseline::AfterEdge:
    case AlignmentBaseline::TextAfterEdge:
    case AlignmentBaseline::Ideographic:
        offset -= font.descent();
        break;
    case AlignmentBaseline::Hanging:
        offset -= font.ascent() * 8.f / 10.f;
        break;
    case AlignmentBaseline::Mathematical:
        offset -= font.ascent() / 2.f;
        break;
    default:
        break;
    }

    return offset;
}

static bool needsTextAnchorAdjustment(const SVGTextPositioningElement* element)
{
    auto direction = element->direction();
    switch(element->text_anchor()) {
    case TextAnchor::Start:
        return direction == Direction::Rtl;
    case TextAnchor::Middle:
        return true;
    case TextAnchor::End:
        return direction == Direction::Ltr;
    default:
        assert(false);
    }

    return false;
}

static float calculateTextAnchorOffset(const SVGTextPositioningElement* element, float width)
{
    auto direction = element->direction();
    switch(element->text_anchor()) {
    case TextAnchor::Start:
        if(direction == Direction::Ltr)
            return 0.f;
        return -width;
    case TextAnchor::Middle:
        return -width / 2.f;
    case TextAnchor::End:
        if(direction == Direction::Ltr)
            return -width;
        return 0.f;
    default:
        assert(false);
    }

    return 0.f;
}

using SVGTextFragmentIterator = SVGTextFragmentList::iterator;

static float calculateTextChunkLength(SVGTextFragmentIterator begin, SVGTextFragmentIterator end, bool isVerticalText)
{
    float chunkLength = 0;
    const SVGTextFragment* lastFragment = nullptr;
    for(auto it = begin; it != end; ++it) {
        const SVGTextFragment& fragment = *it;
        chunkLength += isVerticalText ? fragment.height : fragment.width;
        if(!lastFragment) {
            lastFragment = &fragment;
            continue;
        }

        if(isVerticalText) {
            chunkLength += fragment.y - (lastFragment->y + lastFragment->height);
        } else {
            chunkLength += fragment.x - (lastFragment->x + lastFragment->width);
        }

        lastFragment = &fragment;
    }

    return chunkLength;
}

static void handleTextChunk(SVGTextFragmentIterator begin, SVGTextFragmentIterator end)
{
    const SVGTextFragment& firstFragment = *begin;
    const auto isVerticalText = firstFragment.element->isVerticalWritingMode();
    if(firstFragment.element->hasAttribute(PropertyID::TextLength)) {
        LengthContext lengthContext(firstFragment.element);
        auto textLength = lengthContext.valueForLength(firstFragment.element->textLength());
        auto chunkLength = calculateTextChunkLength(begin, end, isVerticalText);
        if(textLength > 0.f && chunkLength > 0.f) {
            size_t numCharacters = 0;
            for(auto it = begin; it != end; ++it) {
                const SVGTextFragment& fragment = *it;
                numCharacters += fragment.length;
            }

            if(firstFragment.element->lengthAdjust() == LengthAdjust::SpacingAndGlyphs) {
                auto textLengthScale = textLength / chunkLength;
                auto lengthAdjustTransform = Transform::translated(firstFragment.x, firstFragment.y);
                if(isVerticalText) {
                    lengthAdjustTransform.scale(1.f, textLengthScale);
                } else {
                    lengthAdjustTransform.scale(textLengthScale, 1.f);
                }

                lengthAdjustTransform.translate(-firstFragment.x, -firstFragment.y);
                for(auto it = begin; it != end; ++it) {
                    SVGTextFragment& fragment = *it;
                    fragment.lengthAdjustTransform = lengthAdjustTransform;
                }
            } else if(numCharacters > 1) {
                assert(firstFragment.element->lengthAdjust() == LengthAdjust::Spacing);
                size_t characterOffset = 0;
                auto textLengthShift = (textLength - chunkLength) / (numCharacters - 1);
                for(auto it = begin; it != end; ++it) {
                    SVGTextFragment& fragment = *it;
                    if(isVerticalText) {
                        fragment.y += textLengthShift * characterOffset;
                    } else {
                        fragment.x += textLengthShift * characterOffset;
                    }

                    characterOffset += fragment.length;
                }
            }
        }
    }

    if(needsTextAnchorAdjustment(firstFragment.element)) {
        auto chunkLength = calculateTextChunkLength(begin, end, isVerticalText);
        auto chunkOffset = calculateTextAnchorOffset(firstFragment.element, chunkLength);
        for(auto it = begin; it != end; ++it) {
            SVGTextFragment& fragment = *it;
            if(isVerticalText) {
                fragment.y += chunkOffset;
            } else {
                fragment.x += chunkOffset;
            }
        }
    }
}

SVGTextFragmentsBuilder::SVGTextFragmentsBuilder(std::u32string& text, SVGTextFragmentList& fragments)
    : m_text(text), m_fragments(fragments)
{
    m_text.clear();
    m_fragments.clear();
}

void SVGTextFragmentsBuilder::build(const SVGTextElement* textElement)
{
    handleElement(textElement);
    for(const auto& position : m_textPositions) {
        fillCharacterPositions(position);
    }

    std::u32string_view wholeText(m_text);
    for(const auto& textPosition : m_textPositions) {
        if(!textPosition.node->isTextNode())
            continue;
        auto element = toSVGTextPositioningElement(textPosition.node->parentElement());
        const auto isVerticalText = element->isVerticalWritingMode();
        const auto isUprightText = element->isUprightTextOrientation();
        const auto& font = element->font();

        SVGTextFragment fragment(element);
        auto recordTextFragment = [&](auto startOffset, auto endOffset) {
            auto text = wholeText.substr(startOffset, endOffset - startOffset);
            fragment.offset = startOffset;
            fragment.length = text.length();
            fragment.width = font.measureText(text);
            fragment.height = font.height() + font.lineGap();
            if(isVerticalText) {
                m_y += isUprightText ? fragment.height : fragment.width;
            } else {
                m_x += fragment.width;
            }

            m_fragments.push_back(fragment);
        };

        auto needsTextLengthSpacing = element->lengthAdjust() == LengthAdjust::Spacing && element->hasAttribute(PropertyID::TextLength);
        auto baselineOffset = calculateBaselineOffset(element);
        auto startOffset = textPosition.startOffset;
        auto textOffset = textPosition.startOffset;
        auto didStartTextFragment = false;
        auto applySpacingToNextCharacter = false;
        auto lastCharacter = 0u;
        auto lastAngle = 0.f;
        while(textOffset < textPosition.endOffset) {
            SVGCharacterPosition characterPosition;
            if(auto it = m_characterPositions.find(m_characterOffset); it != m_characterPositions.end()) {
                characterPosition = it->second;
            }

            auto currentCharacter = wholeText.at(textOffset);
            auto angle = characterPosition.rotate.value_or(0);
            auto dx = characterPosition.dx.value_or(0);
            auto dy = characterPosition.dy.value_or(0);

            auto shouldStartNewFragment = needsTextLengthSpacing || isVerticalText || applySpacingToNextCharacter
                || characterPosition.x || characterPosition.y || dx || dy || angle || angle != lastAngle;
            if(shouldStartNewFragment && didStartTextFragment) {
                recordTextFragment(startOffset, textOffset);
                applySpacingToNextCharacter = false;
                startOffset = textOffset;
            }

            auto startsNewTextChunk = (characterPosition.x || characterPosition.y) && textOffset == textPosition.startOffset;
            if(startsNewTextChunk || shouldStartNewFragment || !didStartTextFragment) {
                m_x = dx + characterPosition.x.value_or(m_x);
                m_y = dy + characterPosition.y.value_or(m_y);
                fragment.x = isVerticalText ? m_x + baselineOffset : m_x;
                fragment.y = isVerticalText ? m_y : m_y - baselineOffset;
                fragment.angle = angle;
                if(isVerticalText) {
                    if(isUprightText) {
                        fragment.y += font.height();
                    } else {
                        fragment.angle += 90.f;
                    }
                }

                fragment.startsNewTextChunk = startsNewTextChunk;
                didStartTextFragment = true;
            }

            auto spacing = element->letter_spacing();
            if(currentCharacter && lastCharacter && element->word_spacing()) {
                if(currentCharacter == ' ' && lastCharacter != ' ') {
                    spacing += element->word_spacing();
                }
            }

            if(spacing) {
                applySpacingToNextCharacter = true;
                if(isVerticalText) {
                    m_y += spacing;
                } else {
                    m_x += spacing;
                }
            }

            lastAngle = angle;
            lastCharacter = currentCharacter;
            ++textOffset;
            ++m_characterOffset;
        }

        recordTextFragment(startOffset, textOffset);
    }

    if(m_fragments.empty())
        return;
    auto it = m_fragments.begin();
    auto begin = m_fragments.begin();
    auto end = m_fragments.end();
    for(++it; it != end; ++it) {
        const SVGTextFragment& fragment = *it;
        if(!fragment.startsNewTextChunk)
            continue;
        handleTextChunk(begin, it);
        begin = it;
    }

    handleTextChunk(begin, it);
}

void SVGTextFragmentsBuilder::handleText(const SVGTextNode* node)
{
    const auto& text = node->data();
    if(text.empty())
        return;
    auto element = toSVGTextPositioningElement(node->parentElement());
    const auto startOffset = m_text.length();
    uint32_t lastCharacter = ' ';
    if(!m_text.empty()) {
        lastCharacter = m_text.back();
    }

    plutovg_text_iterator_t it;
    plutovg_text_iterator_init(&it, text.data(), text.length(), PLUTOVG_TEXT_ENCODING_UTF8);
    while(plutovg_text_iterator_has_next(&it)) {
        auto currentCharacter = plutovg_text_iterator_next(&it);
        if(currentCharacter == '\t' || currentCharacter == '\n' || currentCharacter == '\r')
            currentCharacter = ' ';
        if(currentCharacter == ' ' && lastCharacter == ' ' && element->white_space() == WhiteSpace::Default)
            continue;
        m_text.push_back(currentCharacter);
        lastCharacter = currentCharacter;
    }

    if(startOffset < m_text.length()) {
        m_textPositions.emplace_back(node, startOffset, m_text.length());
    }
}

void SVGTextFragmentsBuilder::handleElement(const SVGTextPositioningElement* element)
{
    if(element->isDisplayNone())
        return;
    const auto itemIndex = m_textPositions.size();
    m_textPositions.emplace_back(element, m_text.length(), m_text.length());
    for(const auto& child : element->children()) {
        if(child->isTextNode()) {
            handleText(toSVGTextNode(child.get()));
        } else if(child->isTextPositioningElement()) {
            handleElement(toSVGTextPositioningElement(child.get()));
        }
    }

    auto& position = m_textPositions[itemIndex];
    assert(position.node == element);
    position.endOffset = m_text.length();
}

void SVGTextFragmentsBuilder::fillCharacterPositions(const SVGTextPosition& position)
{
    if(!position.node->isTextPositioningElement())
        return;
    auto element = toSVGTextPositioningElement(position.node);
    const auto& xList = element->x();
    const auto& yList = element->y();
    const auto& dxList = element->dx();
    const auto& dyList = element->dy();
    const auto& rotateList = element->rotate();

    auto xListSize = xList.size();
    auto yListSize = yList.size();
    auto dxListSize = dxList.size();
    auto dyListSize = dyList.size();
    auto rotateListSize = rotateList.size();
    if(!xListSize && !yListSize && !dxListSize && !dyListSize && !rotateListSize) {
        return;
    }

    LengthContext lengthContext(element);
    std::optional<float> lastRotation;
    for(auto offset = position.startOffset; offset < position.endOffset; ++offset) {
        auto index = offset - position.startOffset;
        if(index >= xListSize && index >= yListSize && index >= dxListSize && index >= dyListSize && index >= rotateListSize)
            break;
        auto& characterPosition = m_characterPositions[offset];
        if(index < xListSize)
            characterPosition.x = lengthContext.valueForLength(xList[index], LengthDirection::Horizontal);
        if(index < yListSize)
            characterPosition.y = lengthContext.valueForLength(yList[index], LengthDirection::Vertical);
        if(index < dxListSize)
            characterPosition.dx = lengthContext.valueForLength(dxList[index], LengthDirection::Horizontal);
        if(index < dyListSize)
            characterPosition.dy = lengthContext.valueForLength(dyList[index], LengthDirection::Vertical);
        if(index < rotateListSize) {
            characterPosition.rotate = rotateList[index];
            lastRotation = characterPosition.rotate;
        }
    }

    if(lastRotation == std::nullopt)
        return;
    auto offset = position.startOffset + rotateList.size();
    while(offset < position.endOffset) {
        m_characterPositions[offset++].rotate = lastRotation;
    }
}

SVGTextContentElement::SVGTextContentElement(Document* document, ElementID id)
    : SVGGraphicsElement(document, id)
    , m_textLength(PropertyID::TextLength, LengthDirection::Horizontal, LengthNegativeMode::Forbid)
    , m_lengthAdjust(PropertyID::LengthAdjust, LengthAdjust::Spacing)
{
    addProperty(m_textLength);
    addProperty(m_lengthAdjust);
}

void SVGTextContentElement::layoutElement(const SVGLayoutState& state)
{
    m_font = state.font();
    m_fill = getPaintServer(state.fill(), state.fill_opacity());
    m_stroke = getPaintServer(state.stroke(), state.stroke_opacity());
    SVGGraphicsElement::layoutElement(state);

    LengthContext lengthContext(this);
    m_stroke_width = lengthContext.valueForLength(state.stroke_width(), LengthDirection::Diagonal);
    m_letter_spacing = lengthContext.valueForLength(state.letter_spacing(), LengthDirection::Diagonal);
    m_word_spacing = lengthContext.valueForLength(state.word_spacing(), LengthDirection::Diagonal);

    m_baseline_offset = convertBaselineOffset(state.baseline_shift());
    m_alignment_baseline = state.alignment_baseline();
    m_dominant_baseline = state.dominant_baseline();
    m_text_anchor = state.text_anchor();
    m_white_space = state.white_space();
    m_writing_mode = state.writing_mode();
    m_text_orientation = state.text_orientation();
    m_direction = state.direction();
}

float SVGTextContentElement::convertBaselineOffset(const BaselineShift& baselineShift) const
{
    if(baselineShift.type() == BaselineShift::Type::Baseline)
        return 0.f;
    if(baselineShift.type() == BaselineShift::Type::Sub)
        return -m_font.height() / 2.f;
    if(baselineShift.type() == BaselineShift::Type::Super) {
        return m_font.height() / 2.f;
    }

    const auto& length = baselineShift.length();
    if(length.units() == LengthUnits::Percent)
        return length.value() * m_font.size() / 100.f;
    if(length.units() == LengthUnits::Ex)
        return length.value() * m_font.size() / 2.f;
    if(length.units() == LengthUnits::Em)
        return length.value() * m_font.size();
    return length.value();
}

SVGTextPositioningElement::SVGTextPositioningElement(Document* document, ElementID id)
    : SVGTextContentElement(document, id)
    , m_x(PropertyID::X, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_y(PropertyID::Y, LengthDirection::Vertical, LengthNegativeMode::Allow)
    , m_dx(PropertyID::Dx, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_dy(PropertyID::Dy, LengthDirection::Vertical, LengthNegativeMode::Allow)
    , m_rotate(PropertyID::Rotate)
{
    addProperty(m_x);
    addProperty(m_y);
    addProperty(m_dx);
    addProperty(m_dy);
    addProperty(m_rotate);
}

SVGTSpanElement::SVGTSpanElement(Document* document)
    : SVGTextPositioningElement(document, ElementID::Tspan)
{
}

SVGTextPathElement::SVGTextPathElement(Document* document)
    : SVGTextPositioningElement(document, ElementID::TextPath)
    , SVGURIReference(this)
    , m_startOffset(PropertyID::StartOffset, LengthDirection::Horizontal, LengthNegativeMode::Allow)
{
    addProperty(m_startOffset);
}

SVGElement* SVGTextPathElement::targetElement() const
{
    const auto& value = hrefString();
    if(value.empty() || value.front() != '#')
        return nullptr;
    auto element = rootElement()->getElementById(std::string_view(value).substr(1));
    if(element && element->id() == ElementID::Path)
        return element;
    return nullptr;
}

namespace {

struct PathCharacter {
    char32_t character;
    const SVGTextPositioningElement* element;
};

struct PathCharacterAdjust {
    float dx = 0.f;
    float dy = 0.f;
    float rotate = 0.f;
    bool hasRotate = false;
};

// Collects the codepoints of a textPath subtree together with the positioning
// element that owns each one. Whitespace handling mirrors SVGTextFragmentsBuilder.
class TextPathCollector {
public:
    void walk(const SVGTextPathElement* textPath)
    {
        m_textPath = textPath;
        walkElement(textPath);
        fillAdjustments(textPath);
    }

    const std::vector<PathCharacter>& characters() const { return m_characters; }
    const std::map<size_t, PathCharacterAdjust>& adjustments() const { return m_adjustments; }

private:
    void walkElement(const SVGTextPositioningElement* element)
    {
        m_ranges.emplace_back(element, m_characters.size(), m_characters.size());
        for(const auto& child : element->children()) {
            if(child->isTextNode()) {
                collectText(static_cast<const SVGTextNode*>(child.get()), element);
            } else if(auto childElement = toSVGElement(child);
                childElement && childElement->isTextPositioningElement()) {
                walkElement(static_cast<const SVGTextPositioningElement*>(childElement));
            }
        }
        m_ranges.back().end = m_characters.size();
    }

    void collectText(const SVGTextNode* node, const SVGTextPositioningElement* element)
    {
        const auto& text = node->data();
        if(text.empty())
            return;
        plutovg_text_iterator_t it;
        plutovg_text_iterator_init(&it, text.data(), text.length(), PLUTOVG_TEXT_ENCODING_UTF8);
        while(plutovg_text_iterator_has_next(&it)) {
            auto currentCharacter = plutovg_text_iterator_next(&it);
            if(currentCharacter == '\t' || currentCharacter == '\n' || currentCharacter == '\r')
                currentCharacter = ' ';
            if(currentCharacter == ' ' && m_lastCharacter == ' ' && element->white_space() == WhiteSpace::Default)
                continue;
            m_characters.push_back(PathCharacter{static_cast<char32_t>(currentCharacter), element});
            m_lastCharacter = currentCharacter;
        }
    }

    void fillAdjustments(const SVGElement* element)
    {
        if(auto positioning = element->isTextPositioningElement() ? static_cast<const SVGTextPositioningElement*>(element) : nullptr) {
            for(auto& range : m_ranges) {
                if(range.element != positioning)
                    continue;
                const auto& dxList = positioning->dx();
                const auto& dyList = positioning->dy();
                const auto& rotateList = positioning->rotate();
                if(dxList.empty() && dyList.empty() && rotateList.empty())
                    continue;
                LengthContext lengthContext(positioning);
                std::optional<float> lastRotation;
                for(auto offset = range.start; offset < range.end; ++offset) {
                    auto index = offset - range.start;
                    if(index >= dxList.size() && index >= dyList.size() && index >= rotateList.size())
                        break;
                    auto& adjust = m_adjustments[offset];
                    if(index < dxList.size())
                        adjust.dx = lengthContext.valueForLength(dxList[index], LengthDirection::Horizontal);
                    if(index < dyList.size())
                        adjust.dy = lengthContext.valueForLength(dyList[index], LengthDirection::Vertical);
                    if(index < rotateList.size()) {
                        adjust.rotate = rotateList[index];
                        adjust.hasRotate = true;
                        lastRotation = adjust.rotate;
                    }
                }
                if(lastRotation) {
                    for(auto offset = range.start + rotateList.size(); offset < range.end; ++offset) {
                        auto& adjust = m_adjustments[offset];
                        adjust.rotate = *lastRotation;
                        adjust.hasRotate = true;
                    }
                }
            }
        }

        for(const auto& child : element->children()) {
            if(auto childElement = toSVGElement(child))
                fillAdjustments(childElement);
        }
    }

    struct Range {
        const SVGTextPositioningElement* element;
        size_t start;
        size_t end;
        Range(const SVGTextPositioningElement* element, size_t start, size_t end)
            : element(element), start(start), end(end)
        {}
    };

    const SVGTextPathElement* m_textPath = nullptr;
    std::vector<PathCharacter> m_characters;
    std::vector<Range> m_ranges;
    std::map<size_t, PathCharacterAdjust> m_adjustments;
    char32_t m_lastCharacter = ' ';
};

static float glyphAdvance(const Font& font, char32_t character)
{
    std::u32string text(1, static_cast<char32_t>(character));
    return font.measureText(std::u32string_view(text));
}

static Rect glyphExtents(const Font& font, char32_t character)
{
    plutovg_rect_t extents = {0};
    plutovg_font_face_get_glyph_metrics(font.face().get(), font.size(), character, nullptr, nullptr, &extents);
    return Rect(extents.x, extents.y, extents.w, extents.h);
}

// The referenced path may be declared after the text and therefore not yet
// laid out in this pass. Runs the layout state chain down to the target so its
// geometry reflects this round's attributes without involving any sibling.
static void ensureElementLaidOut(SVGElement* target)
{
    std::vector<SVGElement*> chain;
    for(auto element = target; element && !element->isRootElement(); element = element->parentElement())
        chain.push_back(element);

    SVGLayoutState state;
    for(auto it = chain.rbegin(); it != chain.rend(); ++it) {
        SVGLayoutState next(state, *it);
        (*it)->layoutElement(next);
        state = next;
    }
}

} // namespace

SVGTextElement::SVGTextElement(Document* document)
    : SVGTextPositioningElement(document, ElementID::Text)
{
}

void SVGTextElement::layout(SVGLayoutState& state)
{
    SVGTextPositioningElement::layout(state);
    m_fragments.clear();
    m_pathGlyphs.clear();
    m_isTextPath = false;

    const SVGTextPathElement* textPathElement = nullptr;
    for(const auto& child : children()) {
        if(auto element = toSVGElement(child); element && element->isTextPathElement()) {
            textPathElement = static_cast<const SVGTextPathElement*>(element);
            break;
        }
    }

    if(textPathElement) {
        m_isTextPath = true;
        layoutTextPath(textPathElement);
        return;
    }

    SVGTextFragmentsBuilder(m_text, m_fragments).build(this);
}

void SVGTextElement::layoutTextPath(const SVGTextPathElement* textPathElement)
{
    if(textPathElement->isDisplayNone())
        return;
    auto target = textPathElement->targetElement();
    if(target == nullptr)
        return;
    ensureElementLaidOut(target);
    auto geometry = static_cast<const SVGGeometryElement*>(target);
    if(!geometry->isRenderable())
        return;

    // Only the path element's own transform participates; ancestors do not.
    PathMeasure measure(geometry->path(), geometry->localTransform());
    if(!measure.valid())
        return;

    TextPathCollector collector;
    collector.walk(textPathElement);
    const auto& characters = collector.characters();
    if(characters.empty())
        return;

    const auto count = characters.size();
    std::vector<float> baseAdvance(count);
    std::vector<float> spacing(count);
    for(size_t i = 0; i < count; ++i) {
        const auto& font = characters[i].element->font();
        baseAdvance[i] = glyphAdvance(font, characters[i].character);
        auto gap = characters[i].element->letter_spacing();
        if(i > 0 && characters[i].character == ' ' && characters[i - 1].character != ' ')
            gap += characters[i].element->word_spacing();
        spacing[i] = gap;
    }

    // One dimensional advance, spacing, font size and dx accumulation.
    std::vector<float> startDistance(count);
    std::vector<float> normalOffset(count, 0.f);
    float cursor = 0.f;
    for(size_t i = 0; i < count; ++i) {
        if(auto it = collector.adjustments().find(i); it != collector.adjustments().end()) {
            cursor += it->second.dx;
            normalOffset[i] = (i > 0 ? normalOffset[i - 1] : 0.f) + it->second.dy;
        } else if(i > 0) {
            normalOffset[i] = normalOffset[i - 1];
        }
        startDistance[i] = cursor;
        cursor += baseAdvance[i] + spacing[i];
    }
    float contentLength = cursor;
    if(count > 0)
        contentLength -= spacing[count - 1];

    // rotate follows the per-character list with last value continuation.
    std::vector<float> rotation(count, 0.f);
    for(size_t i = 0; i < count; ++i) {
        if(auto it = collector.adjustments().find(i); it != collector.adjustments().end() && it->second.hasRotate)
            rotation[i] = it->second.rotate;
    }

    std::vector<float> scaleX(count, 1.f);
    if(textPathElement->hasAttribute(PropertyID::TextLength) && contentLength > 0.f) {
        LengthContext lengthContext(textPathElement);
        auto textLength = lengthContext.valueForLength(textPathElement->textLength());
        if(textLength > 0.f) {
            if(textPathElement->lengthAdjust() == LengthAdjust::SpacingAndGlyphs) {
                auto k = textLength / contentLength;
                for(size_t i = 0; i < count; ++i) {
                    scaleX[i] = k;
                    spacing[i] *= k;
                    startDistance[i] *= k;
                }
            } else if(count > 1) {
                auto shift = (textLength - contentLength) / float(count - 1);
                for(size_t i = 0; i < count; ++i)
                    startDistance[i] += shift * float(i);
            }
        }
    }

    // Recalculate advances after any glyph scaling.
    std::vector<float> advance(count);
    for(size_t i = 0; i < count; ++i)
        advance[i] = baseAdvance[i] * scaleX[i];

    float totalLength = 0.f;
    for(size_t i = 0; i < count; ++i)
        totalLength += advance[i] + (i + 1 < count ? spacing[i] : 0.f);

    // Text anchoring applies to the whole textPath, never per tspan.
    auto anchorOffset = calculateTextAnchorOffset(textPathElement, totalLength);

    // startOffset: finite unitless length or percentage of the measured path.
    float pathStart = 0.f;
    LengthContext lengthContext(textPathElement);
    const auto& startLength = textPathElement->startOffset();
    if(startLength.value().units() == LengthUnits::Percent)
        pathStart = startLength.value().value() / 100.f * float(measure.length());
    else
        pathStart = lengthContext.valueForLength(startLength.value(), LengthDirection::Horizontal);

    m_pathGlyphs.reserve(count);
    for(size_t i = 0; i < count; ++i) {
        SVGPathGlyph glyph;
        glyph.character = characters[i].character;
        glyph.element = characters[i].element;
        glyph.baseAdvance = baseAdvance[i];
        glyph.advance = advance[i];
        glyph.scaleX = scaleX[i];
        glyph.normalOffset = normalOffset[i];
        glyph.angle = rotation[i];
        glyph.start = pathStart + anchorOffset + startDistance[i];

        auto midpoint = glyph.start + glyph.advance * 0.5f;
        Point position;
        Point tangent;
        if(measure.sample(midpoint, position, tangent)) {
            glyph.visible = true;
            glyph.position = position;
            glyph.tangent = tangent;
            // Downward normal in SVG's y-down coordinate space.
            auto normal = Point(-tangent.y, tangent.x);
            auto baselinePoint = position + Point(normal.x * glyph.normalOffset, normal.y * glyph.normalOffset);
            auto tangentAngle = 180.f * std::atan2(tangent.y, tangent.x) / PLUTOVG_PI;
            // Map font-space (u/2, 0) onto the sampled baseline midpoint.
            glyph.transform = Transform::translated(baselinePoint.x, baselinePoint.y)
                * Transform::rotated(tangentAngle + glyph.angle, 0.f, 0.f)
                * Transform::scaled(glyph.scaleX, 1.f)
                * Transform::translated(-glyph.baseAdvance * 0.5f, 0.f);
        }
        m_pathGlyphs.push_back(glyph);
    }
}

void SVGTextElement::render(SVGRenderState& state) const
{
    if(isVisibilityHidden() || isDisplayNone())
        return;
    if(m_isTextPath) {
        if(m_pathGlyphs.empty())
            return;
    } else if(m_fragments.empty()) {
        return;
    }

    SVGBlendInfo blendInfo(this);
    SVGRenderState newState(this, state, localTransform());
    newState.beginGroup(blendInfo);
    if(newState.mode() == SVGRenderMode::Clipping) {
        newState->setColor(Color::White);
    }

    if(m_isTextPath) {
        for(const auto& glyph : m_pathGlyphs) {
            if(!glyph.visible || glyph.element->isVisibilityHidden())
                continue;
            std::u32string text(1, glyph.character);
            const auto& font = glyph.element->font();
            auto transform = newState.currentTransform() * glyph.transform;
            if(newState.mode() == SVGRenderMode::Clipping) {
                newState->fillText(std::u32string_view(text), font, Point(0.f, 0.f), transform);
            } else {
                const auto& fill = glyph.element->fill();
                const auto& stroke = glyph.element->stroke();
                if(fill.applyPaint(newState))
                    newState->fillText(std::u32string_view(text), font, Point(0.f, 0.f), transform);
                if(stroke.applyPaint(newState))
                    newState->strokeText(std::u32string_view(text), glyph.element->stroke_width(), font, Point(0.f, 0.f), transform);
            }
        }
        newState.endGroup(blendInfo);
        return;
    }

    std::u32string_view wholeText(m_text);
    for(const auto& fragment : m_fragments) {
        if(fragment.element->isVisibilityHidden())
            continue;
        auto transform = newState.currentTransform() * Transform::rotated(fragment.angle, fragment.x, fragment.y) * fragment.lengthAdjustTransform;
        auto text = wholeText.substr(fragment.offset, fragment.length);
        auto origin = Point(fragment.x, fragment.y);

        const auto& font = fragment.element->font();
        if(newState.mode() == SVGRenderMode::Clipping) {
            newState->fillText(text, font, origin, transform);
        } else {
            const auto& fill = fragment.element->fill();
            const auto& stroke = fragment.element->stroke();
            auto stroke_width = fragment.element->stroke_width();
            if(fill.applyPaint(newState))
                newState->fillText(text, font, origin, transform);
            if(stroke.applyPaint(newState)) {
                newState->strokeText(text, stroke_width, font, origin, transform);
            }
        }
    }

    newState.endGroup(blendInfo);
}

Rect SVGTextElement::boundingBox(bool includeStroke) const
{
    auto boundingBox = Rect::Invalid;
    if(m_isTextPath) {
        for(const auto& glyph : m_pathGlyphs) {
            if(!glyph.visible || glyph.element->isVisibilityHidden())
                continue;
            const auto& font = glyph.element->font();
            auto rect = glyphExtents(font, glyph.character);
            if(!rect.isValid() || rect.isEmpty())
                continue;
            if(includeStroke && glyph.element->stroke().isRenderable())
                rect.inflate(glyph.element->stroke_width() / 2.f);
            boundingBox.unite(glyph.transform.mapRect(rect));
        }
        if(!boundingBox.isValid())
            boundingBox = Rect::Empty;
        return boundingBox;
    }

    for(const auto& fragment : m_fragments) {
        const auto& font = fragment.element->font();
        const auto& stroke = fragment.element->stroke();
        auto fragmentTranform = Transform::rotated(fragment.angle, fragment.x, fragment.y) * fragment.lengthAdjustTransform;
        auto fragmentRect = Rect(fragment.x, fragment.y - font.ascent(), fragment.width, fragment.height);
        if(includeStroke && stroke.isRenderable())
            fragmentRect.inflate(fragment.element->stroke_width() / 2.f);
        boundingBox.unite(fragmentTranform.mapRect(fragmentRect));
    }

    if(!boundingBox.isValid())
        boundingBox = Rect::Empty;
    return boundingBox;
}

} // namespace lunasvg
