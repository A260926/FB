// https://developer.mozilla.org/en-US/docs/Web/HTML/Block-level_elements
const Htags = ['h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hgroup'];
const Dtags = ['details', 'dialog', 'dd', 'div', 'dt'];
const Ftags = ['fieldset', 'figcaption', 'figure', 'footer', 'form'];
const tableTags = ['table', 'td', 'tr'];
const htmlTags = ['address', 'article', 'aside', 'blockquote', 'br', 'hr', 'li', 'main', 'nav', 'ol', 'p', 'pre', 'section', 'ul'];

/** Block-level element names in both original and upper-cased form. */
const kBlockElements = new Set<string>();
for (const tags of [Htags, Dtags, Ftags, tableTags, htmlTags]) {
	for (const element of tags) {
		kBlockElements.add(element);
		kBlockElements.add(element.toUpperCase());
	}
}

export default kBlockElements;
