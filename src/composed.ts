import { selectAll } from 'css-select';
import { decodeHTML } from 'entities';
import Matcher from './matcher';
import type HTMLElement from './nodes/html';
import type Node from './nodes/node';
import NodeType from './nodes/type';

/**
 * Composed (flattened) view over declarative shadow DOM, for offline scraping.
 *
 * The view is computed on the fly and never touches the physical tree:
 * `childNodes`, `parentNode`, serialization and the classic query/text APIs
 * keep seeing the parsed document exactly as it was.
 *
 * Expansion rules (static HTML only, no script execution):
 * - The first direct-child `<template shadowrootmode="open">` of an element is
 *   its shadow template. The host stays in place, but its composed children
 *   come from the template content instead of its light DOM.
 * - Every other template (closed, non-first, or without `shadowrootmode`) is
 *   inert and does not appear in the view.
 * - A `<slot>` inside the open template is a transparent placeholder: the
 *   host's direct light DOM children (elements keyed by their `slot`
 *   attribute, text keyed by the default name) take the slot's position.
 *   Only the first slot of each name in template tree order receives assigned
 *   content; unassigned and duplicate slots show their own fallback children.
 *   Light DOM that no slot claims is not displayed. Comments never take part.
 * - Nested hosts are expanded by their own template and light DOM when the
 *   traversal reaches them; the outer template walk stops at their boundary,
 *   so no node can appear twice.
 */

/**
 * Slot assignment and primary-slot bookkeeping for one host template.
 * Rebuilt on every use, so mutations (slot attributes, moved light DOM,
 * replaced template content) are reflected on the next call.
 */
interface TemplateContext {
	/** slot name -> assigned light DOM nodes, in document order */
	assignment: Map<string, Node[]>;
	/** the first slot element of each name in template tree order */
	primaries: Set<HTMLElement>;
	/** slot name -> primary slot element */
	primaryByName: Map<string, HTMLElement>;
}

type ComposedContext = TemplateContext | null;

function tagName(node: Node): string {
	return node.nodeType === NodeType.ELEMENT_NODE ? ((node as HTMLElement).rawTagName || '').toLowerCase() : '';
}

function isTemplateElement(node: Node): node is HTMLElement {
	return tagName(node) === 'template';
}

function isSlotElement(node: Node): node is HTMLElement {
	return tagName(node) === 'slot';
}

/**
 * The first direct-child template with `shadowrootmode="open"`, if any.
 */
function getOpenShadowTemplate(node: Node): HTMLElement | null {
	for (const child of node.childNodes) {
		if (isTemplateElement(child) && (child.getAttribute('shadowrootmode') || '').toLowerCase() === 'open') {
			return child;
		}
	}
	return null;
}

/**
 * Nodes that may appear in the composed view: elements (except templates,
 * which are always inert) and text. Comments never participate.
 */
function isVisibleNode(node: Node): boolean {
	return (node.nodeType === NodeType.ELEMENT_NODE && !isTemplateElement(node)) || node.nodeType === NodeType.TEXT_NODE;
}

/**
 * Name used for slot assignment: the `slot` attribute for elements,
 * the default (empty) name for text nodes.
 */
function assignmentNameOf(node: Node): string {
	return node.nodeType === NodeType.ELEMENT_NODE ? (node as HTMLElement).getAttribute('slot') || '' : '';
}

function slotElementName(slot: HTMLElement): string {
	return slot.getAttribute('name') || '';
}

function makeTemplateContext(host: HTMLElement, template: HTMLElement): TemplateContext {
	// Direct light DOM children of the host, keyed by slot name. Templates are
	// inert and comments do not participate; everything else (including
	// whitespace-only text) counts as assignable content.
	const assignment = new Map<string, Node[]>();
	for (const child of host.childNodes) {
		if (!isVisibleNode(child)) continue;
		const name = assignmentNameOf(child);
		const list = assignment.get(name);
		if (list) {
			list.push(child);
		} else {
			assignment.set(name, [child]);
		}
	}
	// First slot of each name in template tree order. The walk enters neither
	// nested hosts (their own template and light DOM are handled when they are
	// expanded) nor inert templates. Slot fallback content is part of the
	// template tree, so slots inside it do count for the order.
	const primaries = new Set<HTMLElement>();
	const primaryByName = new Map<string, HTMLElement>();
	const stack: Node[] = [];
	for (let i = template.childNodes.length - 1; i >= 0; i--) stack.push(template.childNodes[i]);
	while (stack.length > 0) {
		const node = stack.pop() as Node;
		if (node.nodeType !== NodeType.ELEMENT_NODE || isTemplateElement(node)) continue;
		const element = node as HTMLElement;
		if (isSlotElement(element)) {
			const name = slotElementName(element);
			if (!primaryByName.has(name)) {
				primaryByName.set(name, element);
				primaries.add(element);
			}
		} else if (getOpenShadowTemplate(element)) {
			continue; // nested host boundary
		}
		for (let i = element.childNodes.length - 1; i >= 0; i--) stack.push(element.childNodes[i]);
	}
	return { assignment, primaries, primaryByName };
}

/**
 * Emits one level of template content, resolving slots against the host's
 * assignment. Slots are transparent: assigned light DOM (or, for unassigned
 * and duplicate slots, the fallback children) is emitted in their place.
 */
function expandTemplateLevel(children: Node[], ctx: TemplateContext, emit: (node: Node, ctx: ComposedContext) => void): void {
	// Iterative, so that deeply nested fallback chains cannot overflow the stack.
	const stack: Node[] = [];
	for (let i = children.length - 1; i >= 0; i--) stack.push(children[i]);
	while (stack.length > 0) {
		const node = stack.pop() as Node;
		if (!isVisibleNode(node)) continue;
		if (isSlotElement(node)) {
			const assigned = ctx.primaries.has(node) ? ctx.assignment.get(slotElementName(node)) : undefined;
			if (assigned && assigned.length > 0) {
				// First slot of this name: assigned light DOM takes its place.
				for (const assignedNode of assigned) emit(assignedNode, null);
			} else {
				// Unassigned or duplicate slot: show its own fallback children,
				// which are still template content of the same template.
				for (let i = node.childNodes.length - 1; i >= 0; i--) stack.push(node.childNodes[i]);
			}
			continue;
		}
		if (node.nodeType === NodeType.ELEMENT_NODE) {
			// Nested hosts expand by their own template when traversed; any
			// other element keeps expanding in this template context.
			emit(node, getOpenShadowTemplate(node) ? null : ctx);
			continue;
		}
		emit(node, null); // text node
	}
}

/**
 * Computes the composed children of a node and hands each to `emit` together
 * with the context needed to expand that child later on.
 */
function computeComposedChildren(node: Node, ctx: ComposedContext, emit: (node: Node, ctx: ComposedContext) => void): void {
	if (node.nodeType !== NodeType.ELEMENT_NODE) return;
	const element = node as HTMLElement;
	const template = getOpenShadowTemplate(element);
	if (template) {
		// Host: the composed children come from the open template, not the light DOM.
		expandTemplateLevel(template.childNodes, makeTemplateContext(element, template), emit);
		return;
	}
	if (ctx) {
		// The node lives inside template content: slots among its children
		// resolve against the owning host's assignment.
		expandTemplateLevel(element.childNodes, ctx, emit);
		return;
	}
	for (const child of element.childNodes) {
		if (isVisibleNode(child)) emit(child, null);
	}
}

/**
 * Derives the expansion context of a node from its physical ancestry. Only
 * needed for the root of a composed traversal; every other node gets its
 * context from the expansion that emitted it.
 */
function deriveContext(node: Node): ComposedContext {
	let current: Node | null = node.parentNode;
	while (current) {
		if (isTemplateElement(current)) {
			const host = current.parentNode;
			// Inside the host's open template: template context. Any other
			// template is inert, so its content is traversed as-is.
			return host && getOpenShadowTemplate(host) === current ? makeTemplateContext(host, current) : null;
		}
		if (getOpenShadowTemplate(current)) return null; // light DOM of a host
		current = current.parentNode;
	}
	return null;
}

/**
 * Composed-view parent of a node, computed from the physical tree. Used for
 * the root of a traversal; every node below it knows its composed parent from
 * the expansion that emitted it.
 */
function composedParentOf(node: Node): Node | null {
	if (!isVisibleNode(node)) return null;
	let current = node;
	while (true) {
		const parent: Node | null = current.parentNode;
		if (!parent) return null;
		if (isTemplateElement(parent)) {
			const host = parent.parentNode;
			// Content of the host's open template belongs to the host; anything
			// inside an inert template is not part of the composed view.
			return host && getOpenShadowTemplate(host) === parent ? host : null;
		}
		if (isSlotElement(parent)) {
			const ctx = deriveContext(parent);
			if (ctx) {
				// Fallback content is only visible while its slot shows the fallback.
				const assigned = ctx.primaries.has(parent) ? ctx.assignment.get(slotElementName(parent)) : undefined;
				if (assigned && assigned.length > 0) return null;
				current = parent; // slots are transparent: continue from the slot's position
				continue;
			}
			// A <slot> outside template content is an ordinary element.
		}
		if (getOpenShadowTemplate(parent)) {
			// Direct light DOM child of a host: visible only when a slot claims it.
			if (!isVisibleNode(current)) return null;
			const template = getOpenShadowTemplate(parent) as HTMLElement;
			const slot = makeTemplateContext(parent as HTMLElement, template).primaryByName.get(assignmentNameOf(current));
			if (!slot) return null;
			current = slot; // assigned content sits at the slot's position
			continue;
		}
		return parent;
	}
}

export interface ComposedNavigator {
	getChildren(node: Node): Node[];
	getParent(node: Node): Node | null;
	getSiblings(node: Node): Node[];
	getText(node: Node): string;
}

/**
 * Composed-view relations over the physical tree. The navigator is stateless
 * with respect to the document: results are cached per navigator for the
 * duration of one query/text read, and every new navigator recomputes
 * everything from the current tree.
 */
export function createComposedNavigator(): ComposedNavigator {
	const childrenCache = new WeakMap<Node, Node[]>();
	const contextCache = new WeakMap<Node, ComposedContext>();
	const parentCache = new WeakMap<Node, Node>();

	function getChildren(node: Node): Node[] {
		const cached = childrenCache.get(node);
		if (cached) return cached;
		const children: Node[] = [];
		const ctx = contextCache.has(node) ? contextCache.get(node) : deriveContext(node);
		computeComposedChildren(node, ctx as ComposedContext, (child, childCtx) => {
			parentCache.set(child, node);
			contextCache.set(child, childCtx);
			children.push(child);
		});
		childrenCache.set(node, children);
		return children;
	}

	function getParent(node: Node): Node | null {
		return parentCache.get(node) || composedParentOf(node);
	}

	function getSiblings(node: Node): Node[] {
		const parent = getParent(node);
		return parent ? getChildren(parent) : [];
	}

	function getText(node: Node): string {
		if (node.nodeType === NodeType.TEXT_NODE) return node.text;
		if (node.nodeType !== NodeType.ELEMENT_NODE) return '';
		// Same text as HTMLElement#text, but collected over the composed view.
		const parts: string[] = [];
		const stack: Node[] = [node];
		while (stack.length > 0) {
			const current = stack.pop() as Node;
			if (current.nodeType === NodeType.TEXT_NODE) {
				parts.push(current.rawText);
			} else if (current.nodeType === NodeType.ELEMENT_NODE) {
				if (/^br$/i.test((current as HTMLElement).rawTagName)) {
					parts.push('\n');
					continue;
				}
				const children = getChildren(current);
				for (let i = children.length - 1; i >= 0; i--) stack.push(children[i]);
			}
		}
		return decodeHTML(parts.join(''));
	}

	return { getChildren, getParent, getSiblings, getText };
}

/**
 * Minimal Adapter interface matching css-select's Adapter<Node, ElementNode>,
 * mirroring the one in matcher.ts.
 */
interface Adapter {
	isTag: (node: Node) => node is HTMLElement;
	existsOne: (test: (elem: HTMLElement) => boolean, elems: Node[]) => boolean;
	getAttributeValue: (elem: HTMLElement, name: string) => string | undefined;
	getChildren: (node: Node) => Node[];
	getName: (elem: HTMLElement) => string;
	getParent: (node: HTMLElement) => Node | null;
	getSiblings: (node: Node) => Node[];
	getText: (node: Node) => string;
	hasAttrib: (elem: HTMLElement, name: string) => boolean;
	removeSubsets: (nodes: Node[]) => Node[];
	findAll: (test: (elem: HTMLElement) => boolean, nodes: Node[]) => HTMLElement[];
	findOne: (test: (elem: HTMLElement) => boolean, elems: Node[]) => HTMLElement | null;
}

/**
 * Like {@link HTMLElement.querySelectorAll}, but evaluated against the
 * composed view: parent/child, ancestor, sibling and positional relations all
 * follow the expanded tree, in which `<slot>` and `<template>` occupy no
 * position. Returned elements are the very objects of the physical tree.
 */
export function selectAllComposed(root: HTMLElement, selector: string): HTMLElement[] {
	const navigator = createComposedNavigator();
	const { getChildren, getParent, getSiblings, getText } = navigator;

	function isTag(node: Node): node is HTMLElement {
		return node && node.nodeType === NodeType.ELEMENT_NODE;
	}

	function removeSubsets(nodes: Node[]) {
		let idx = nodes.length;
		let node;
		let ancestor;
		let replace;

		// Check if each node (or one of its ancestors) is already contained in
		// the array.
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
				ancestor = getParent(ancestor);
			}

			// If the node has been found to be unique, re-insert it.
			if (replace) {
				nodes[idx] = node;
			}
		}

		return nodes;
	}

	function existsOne(test: (elem: HTMLElement) => boolean, elems: Node[]): boolean {
		const stack: Node[] = [];
		for (let i = elems.length - 1; i >= 0; i--) stack.push(elems[i]);
		while (stack.length > 0) {
			const elem = stack.pop() as Node;
			if (!isTag(elem)) continue;
			if (test(elem)) return true;
			const children = getChildren(elem);
			for (let i = children.length - 1; i >= 0; i--) stack.push(children[i]);
		}
		return false;
	}

	function findOne(test: (elem: HTMLElement) => boolean, elems: Node[]) {
		const stack: Node[] = [];
		for (let i = elems.length - 1; i >= 0; i--) stack.push(elems[i]);
		while (stack.length > 0) {
			const el = stack.pop() as Node;
			if ((test as (node: Node) => boolean)(el)) {
				return el as HTMLElement;
			}
			const children = getChildren(el);
			for (let i = children.length - 1; i >= 0; i--) stack.push(children[i]);
		}
		return null;
	}

	function findAll(test: (elem: HTMLElement) => boolean, nodes: Node[]): HTMLElement[] {
		const result: HTMLElement[] = [];
		const stack: Node[] = [];
		for (let i = nodes.length - 1; i >= 0; i--) stack.push(nodes[i]);

		while (stack.length > 0) {
			const node = stack.pop() as Node;
			if (!isTag(node)) continue;
			if (test(node)) result.push(node);
			const children = getChildren(node);
			for (let i = children.length - 1; i >= 0; i--) stack.push(children[i]);
		}

		return result;
	}

	const adapter: Adapter = {
		...Matcher,
		getChildren,
		getParent,
		getSiblings,
		getText,
		removeSubsets,
		existsOne,
		findOne,
		findAll
	};

	return selectAll(selector, root, {
		xmlMode: true,
		adapter
	});
}
