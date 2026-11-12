import kBlockElements from './block-elements';
import Matcher from './matcher';
import type HTMLElement from './nodes/html';
import type Node from './nodes/node';
import type TextNode from './nodes/text';
import NodeType from './nodes/type';

/**
 * Composed (declarative Shadow DOM) view for static, script-free HTML.
 *
 * The physical tree parsed from the HTML is never rewritten: every node handed
 * out by this module is an object that already lives in the physical tree, and
 * neither childNodes nor parentNode are ever mutated. Instead, a lazily-built,
 * per-query view describes which children an element presents once its first
 * open `<template shadowrootmode="open">` has been expanded with slot
 * assignment.
 *
 * A new view is built on every call, so mutating slot attributes, moving light
 * DOM around or replacing a template's content is reflected by the next call;
 * node identities returned by earlier calls stay valid against the physical
 * tree.
 */

const DEFAULT_SLOT = '';
const SHADOW_ROOT_MODE = 'shadowrootmode';

interface ComposedAdapter {
	isTag: (node: Node) => node is HTMLElement;
	existsOne: (test: (elem: HTMLElement) => boolean, elems: Node[]) => boolean;
	getAttributeValue: (elem: HTMLElement, name: string) => string | undefined;
	getChildren: (node: Node) => Node[];
	getName: (elem: HTMLElement) => string;
	getParent: (node: Node) => Node | null;
	getSiblings: (node: Node) => Node[];
	getText: (node: Node) => string;
	hasAttrib: (elem: HTMLElement, name: string) => boolean;
	removeSubsets: (nodes: Node[]) => Node[];
	findAll: (test: (elem: HTMLElement) => boolean, nodes: Node[]) => HTMLElement[];
	findOne: (test: (elem: HTMLElement) => boolean, elems: Node[]) => HTMLElement | null;
}

function isTemplate(node: Node): node is HTMLElement {
	return node.nodeType === NodeType.ELEMENT_NODE && (node as HTMLElement).rawTagName.toLowerCase() === 'template';
}

function isSlot(node: Node): node is HTMLElement {
	return node.nodeType === NodeType.ELEMENT_NODE && (node as HTMLElement).rawTagName.toLowerCase() === 'slot';
}

/**
 * The first direct open shadow-root template of a host, if any. Only that
 * template is expanded; every other template stays inert content.
 */
function findOpenTemplate(host: HTMLElement): HTMLElement | null {
	for (const child of host.childNodes) {
		if (isTemplate(child) && child.getAttribute(SHADOW_ROOT_MODE) === 'open') {
			return child;
		}
	}
	return null;
}

function slotName(node: Node): string {
	if (node.nodeType === NodeType.ELEMENT_NODE) {
		// Elements without a slot attribute, and text nodes, land in the
		// default slot. Comments never participate in assignment.
		return (node as HTMLElement).getAttribute('slot') || DEFAULT_SLOT;
	}
	return DEFAULT_SLOT;
}

/**
 * Builds the slot-name -> assigned-nodes map of a host from its direct light
 * DOM. The host's own open template (and any ordinary template) is not entered
 * here: inner hosts are expanded on their own when the view reaches them.
 */
function collectAssignments(host: HTMLElement): Map<string, Node[]> {
	const assigned = new Map<string, Node[]>();
	for (const child of host.childNodes) {
		if (child.nodeType === NodeType.COMMENT_NODE || isTemplate(child)) {
			// Comments do not participate; the shadow template is not light DOM
			// and other templates are inert.
			continue;
		}
		const name = slotName(child);
		let bucket = assigned.get(name);
		if (!bucket) {
			bucket = [];
			assigned.set(name, bucket);
		}
		bucket.push(child);
	}
	return assigned;
}

export class ComposedView {
	private readonly childrenOf = new Map<Node, Node[]>();
	private readonly parentOf = new Map<Node, Node>();

	/** View children of a node: templates and slot elements never occupy a position themselves. */
	public getChildren(node: Node): Node[] {
		const cached = this.childrenOf.get(node);
		if (cached) {
			return cached;
		}
		const children = this.buildChildren(node);
		this.childrenOf.set(node, children);
		return children;
	}

	public getParent(node: Node): Node | null {
		if (this.parentOf.has(node)) {
			return this.parentOf.get(node);
		}
		// Nodes that never appear in the composed view (unassigned light DOM,
		// template internals) keep their physical parent.
		return node.parentNode;
	}

	public getSiblings(node: Node): Node[] {
		const parent = this.getParent(node);
		return parent ? this.getChildren(parent) : [];
	}

	/**
	 * Text of a node as presented by the composed view (decoded, like
	 * {@link HTMLElement.text}), so `:contains()` and `:empty` read the same
	 * tree the selectors traverse.
	 */
	public getText(node: Node): string {
		if (node.nodeType !== NodeType.ELEMENT_NODE) {
			return node.text;
		}
		const out: string[] = [];
		const walk = (current: Node) => {
			if (current.nodeType !== NodeType.ELEMENT_NODE) {
				out.push(current.text);
				return;
			}
			for (const child of this.getChildren(current)) {
				walk(child);
			}
		};
		walk(node);
		return out.join('');
	}

	public toAdapter(): ComposedAdapter {
		const view = this;
		function removeSubsets(nodes: Node[]) {
			let idx = nodes.length;
			let node: Node;
			let ancestor: Node;
			let replace: boolean;
			// Check if each node (or one of its ancestors) is already contained in the
			// array.
			while (--idx > -1) {
				node = ancestor = nodes[idx];
				// Temporarily remove the node under consideration
				nodes[idx] = null;
				replace = true;
				while (ancestor) {
					if (nodes.indexOf(ancestor) > -1) {
						replace = false;
						nodes.splice(idx, 1);
						break;
					}
					ancestor = view.getParent(ancestor);
				}
				// If the node has been found to be unique, re-insert it.
				if (replace) {
					nodes[idx] = node;
				}
			}
			return nodes;
		}
		function existsOne(test: (elem: HTMLElement) => boolean, elems: Node[]): boolean {
			return elems.some((elem) => {
				return Matcher.isTag(elem) ? test(elem) || existsOne(test, view.getChildren(elem)) : false;
			});
		}
		function findOne(test: (elem: HTMLElement) => boolean, elems: Node[]): HTMLElement | null {
			let elem = null as HTMLElement | null;
			for (let i = 0, l = elems?.length; i < l && !elem; i++) {
				const el = elems[i];
				if ((test as (node: Node) => boolean)(el)) {
					elem = el as HTMLElement;
				} else {
					const childs = view.getChildren(el);
					if (childs && childs.length > 0) {
						elem = findOne(test, childs);
					}
				}
			}
			return elem;
		}
		function findAll(test: (elem: HTMLElement) => boolean, nodes: Node[]): HTMLElement[] {
			const result: HTMLElement[] = [];
			const stack: Node[] = [];
			for (let i = nodes.length - 1; i >= 0; i--) {
				stack.push(nodes[i]);
			}
			while (stack.length > 0) {
				const node = stack.pop();
				if (!Matcher.isTag(node)) {
					continue;
				}
				if (test(node)) {
					result.push(node);
				}
				const childs = view.getChildren(node);
				if (childs) {
					for (let i = childs.length - 1; i >= 0; i--) {
						stack.push(childs[i]);
					}
				}
			}
			return result;
		}
		return {
			isTag: Matcher.isTag,
			getAttributeValue: Matcher.getAttributeValue,
			getName: Matcher.getName,
			hasAttrib: Matcher.hasAttrib,
			getChildren: (node: Node) => view.getChildren(node),
			getParent: (node: Node) => view.getParent(node),
			getSiblings: (node: Node) => view.getSiblings(node),
			getText: (node: Node) => view.getText(node),
			removeSubsets,
			existsOne,
			findOne,
			findAll
		};
	}

	private buildChildren(node: Node): Node[] {
		if (node.nodeType !== NodeType.ELEMENT_NODE) {
			return node.childNodes;
		}
		const element = node as HTMLElement;
		const template = findOpenTemplate(element);
		if (!template) {
			// No open shadow root: ordinary traversal of the non-template
			// children. Any template here is inert content.
			return element.childNodes.filter((child) => !isTemplate(child));
		}
		const assigned = collectAssignments(element);
		const taken = new Set<string>();
		const emitted: Node[] = [];
		this.registerShadowContent(template, element, assigned, taken, emitted);
		return emitted;
	}

	/**
	 * Walks one shadow template's content and registers the view children of
	 * every element encountered, substituting slots in place. Nested hosts
	 * (elements that own their own open template) are emitted but never
	 * entered: they are expanded lazily, from their own light DOM and
	 * template, when the view reaches them.
	 */
	private registerShadowContent(
		container: Node,
		viewParent: Node,
		assigned: Map<string, Node[]>,
		taken: Set<string>,
		emitted: Node[]
	): void {
		for (const child of container.childNodes) {
			if (isSlot(child)) {
				const name = child.getAttribute('name') || DEFAULT_SLOT;
				const bucket = assigned.get(name);
				if (bucket && !taken.has(name)) {
					// The first slot of a name receives every node assigned to
					// it; even a whitespace-only assignment suppresses the
					// slot's fallback content.
					taken.add(name);
					for (const assignedNode of bucket) {
						this.relate(assignedNode, viewParent, emitted);
					}
				} else {
					// Unassigned or repeated same-name slots keep their own
					// fallback children; the slot itself stays transparent.
					this.registerShadowContent(child, viewParent, assigned, taken, emitted);
				}
				continue;
			}
			if (isTemplate(child)) {
				// Inert templates take no position; comments stay in the view
				// just as they do for elements without a shadow root.
				continue;
			}
			if (child.nodeType === NodeType.ELEMENT_NODE) {
				this.relate(child, viewParent, emitted);
				if (!findOpenTemplate(child as HTMLElement)) {
					const innerEmitted: Node[] = [];
					this.registerShadowContent(child, child, assigned, taken, innerEmitted);
					this.childrenOf.set(child, innerEmitted);
				}
				continue;
			}
			this.relate(child, viewParent, emitted);
		}
	}

	private relate(node: Node, viewParent: Node, out: Node[]): void {
		this.parentOf.set(node, viewParent);
		out.push(node);
	}
}

/**
 * Structured text of {@link root} read from a freshly built composed view.
 * The block boundaries, entity decoding and whitespace normalization mirror
 * {@link HTMLElement.structuredText}; only the traversed tree changes.
 */
export function getStructuredTextComposed(root: HTMLElement): string {
	interface ICurrentBlock extends Array<string> {
		prependWhitespace?: boolean;
	}
	const view = new ComposedView();
	let currentBlock = [] as ICurrentBlock;
	const blocks = [currentBlock];
	function dfs(node: Node) {
		if (node.nodeType === NodeType.ELEMENT_NODE) {
			const element = node as HTMLElement;
			if (kBlockElements.has(element.rawTagName)) {
				if (currentBlock.length > 0) {
					blocks.push((currentBlock = []));
				}
				view.getChildren(node).forEach(dfs);
				if (currentBlock.length > 0) {
					blocks.push((currentBlock = []));
				}
			} else {
				view.getChildren(node).forEach(dfs);
			}
		} else if (node.nodeType === NodeType.TEXT_NODE) {
			const textNode = node as TextNode;
			if (textNode.isWhitespace) {
				// Whitespace node, postponed output (only meaningful between
				// two pieces of text on the same line, never at the start of a block)
				if (currentBlock.length > 0) {
					currentBlock.prependWhitespace = true;
				}
			} else {
				let text = textNode.trimmedText;
				if (currentBlock.prependWhitespace) {
					text = ` ${text}`;
					currentBlock.prependWhitespace = false;
				}
				currentBlock.push(text);
			}
		}
	}
	dfs(root);
	return blocks
		.map((block) => {
			return block.join('').replace(/\s{2,}/g, ' ').trim(); // Normalize each line's whitespace and trim it
		})
		.join('\n')
		.replace(/\s+$/, ''); // trimRight;
}
