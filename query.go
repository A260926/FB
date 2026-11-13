package agouti

import (
	"errors"
	"fmt"
	"strings"

	"github.com/sclevine/agouti/api"
	"github.com/sclevine/agouti/internal/element"
)

type queryKind int

const (
	queryInvalid queryKind = iota
	queryCSS
	queryThen
	queryHas
	queryHasNot
	queryOr
	queryAt
)

// Query is an immutable element-query expression.
//
// Queries are evaluated against an element context using WebDriver's
// element-scoped find-elements command. They never execute page JavaScript and
// never retrieve the page source.
//
// Query values are always copied when they are constructed or combined. A
// Query may therefore be reused across selections, pages, and sessions, and
// combining a Query never changes an existing expression.
type Query struct {
	kind  queryKind
	css   string
	left  *Query
	right *Query
	index int
}

// QueryCSS returns a Query that finds all descendants of the current element
// context matching the provided CSS selector. Results are returned in the
// order reported by the WebDriver server.
func QueryCSS(selector string) Query {
	return Query{kind: queryCSS, css: selector}
}

// Then returns a Query that applies the provided Query to every element found
// by the receiver. Results are concatenated in receiver-result order and are
// not de-duplicated across the receiver's elements.
func (q Query) Then(other Query) Query {
	left, right := q, other
	return Query{kind: queryThen, left: &left, right: &right}
}

// Has returns a Query that keeps every element found by the receiver for which
// the provided Query finds at least one result rooted at that element.
func (q Query) Has(other Query) Query {
	left, right := q, other
	return Query{kind: queryHas, left: &left, right: &right}
}

// HasNot returns a Query that keeps every element found by the receiver for
// which the provided Query finds no results rooted at that element. A remote
// error is never treated as an absence of results.
func (q Query) HasNot(other Query) Query {
	left, right := q, other
	return Query{kind: queryHasNot, left: &left, right: &right}
}

// Or returns a Query that evaluates both the receiver and the provided Query
// against the same input context. Receiver results come first, followed by
// results from the other Query that have not already appeared. Elements are
// de-duplicated by their remote element ID within that input context.
func (q Query) Or(other Query) Query {
	left, right := q, other
	return Query{kind: queryOr, left: &left, right: &right}
}

// At returns a Query that selects a single zero-based result from the complete
// expression that immediately precedes it. An out-of-range index fails when the
// query is evaluated. A negative index is an invalid expression.
func (q Query) At(index int) Query {
	left := q
	return Query{kind: queryAt, left: &left, index: index}
}

// String returns a human-readable representation of the Query. It never
// contacts the WebDriver server.
func (q Query) String() string {
	switch q.kind {
	case queryCSS:
		return fmt.Sprintf("CSS: %s", q.css)
	case queryThen:
		return fmt.Sprintf("%s.Then(%s)", q.left, q.right)
	case queryHas:
		return fmt.Sprintf("%s.Has(%s)", q.left, q.right)
	case queryHasNot:
		return fmt.Sprintf("%s.HasNot(%s)", q.left, q.right)
	case queryOr:
		return fmt.Sprintf("%s.Or(%s)", q.left, q.right)
	case queryAt:
		return fmt.Sprintf("%s.At(%d)", q.left, q.index)
	}
	return "<invalid query>"
}

func (q *Query) validate() error {
	switch q.kind {
	case queryCSS:
		if strings.TrimSpace(q.css) == "" {
			return errors.New("query CSS selector must not be empty")
		}
	case queryThen, queryHas, queryHasNot, queryOr:
		if q.left == nil || q.right == nil {
			return errors.New("invalid zero-value query")
		}
		if err := q.left.validate(); err != nil {
			return err
		}
		return q.right.validate()
	case queryAt:
		if q.left == nil {
			return errors.New("invalid zero-value query")
		}
		if q.index < 0 {
			return fmt.Errorf("query index must not be negative (%d)", q.index)
		}
		return q.left.validate()
	default:
		return errors.New("invalid zero-value query")
	}
	return nil
}

// evalQuery evaluates a query rooted at a single element context.
func evalQuery(q Query, root element.Client) ([]element.Element, error) {
	switch q.kind {
	case queryCSS:
		results, err := root.GetElements(api.Selector{Using: "css selector", Value: q.css})
		if err != nil {
			return nil, err
		}
		elements := make([]element.Element, 0, len(results))
		for _, result := range results {
			elements = append(elements, result)
		}
		return elements, nil

	case queryThen:
		left, err := evalQuery(*q.left, root)
		if err != nil {
			return nil, err
		}
		results := []element.Element{}
		for _, selected := range left {
			right, err := evalQuery(*q.right, selected)
			if err != nil {
				return nil, err
			}
			results = append(results, right...)
		}
		return results, nil

	case queryHas:
		left, err := evalQuery(*q.left, root)
		if err != nil {
			return nil, err
		}
		results := []element.Element{}
		for _, selected := range left {
			right, err := evalQuery(*q.right, selected)
			if err != nil {
				return nil, err
			}
			if len(right) > 0 {
				results = append(results, selected)
			}
		}
		return results, nil

	case queryHasNot:
		left, err := evalQuery(*q.left, root)
		if err != nil {
			return nil, err
		}
		results := []element.Element{}
		for _, selected := range left {
			right, err := evalQuery(*q.right, selected)
			if err != nil {
				return nil, err
			}
			if len(right) == 0 {
				results = append(results, selected)
			}
		}
		return results, nil

	case queryOr:
		left, err := evalQuery(*q.left, root)
		if err != nil {
			return nil, err
		}
		right, err := evalQuery(*q.right, root)
		if err != nil {
			return nil, err
		}

		results := append([]element.Element{}, left...)
		seen := map[string]bool{}
		for _, selected := range left {
			seen[selected.GetID()] = true
		}
		for _, selected := range right {
			if seen[selected.GetID()] {
				continue
			}
			seen[selected.GetID()] = true
			results = append(results, selected)
		}
		return results, nil

	case queryAt:
		left, err := evalQuery(*q.left, root)
		if err != nil {
			return nil, err
		}
		if q.index >= len(left) {
			return nil, fmt.Errorf("query index %d out of range (found %d elements)", q.index, len(left))
		}
		return []element.Element{left[q.index]}, nil
	}

	return nil, errors.New("invalid zero-value query")
}
