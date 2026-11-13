package agouti

import (
	"errors"
	"fmt"
	"strings"

	"github.com/sclevine/agouti/internal/element"
	"github.com/sclevine/agouti/internal/target"
)

// queryPlanSegment is a single stage of a selection plan. A segment is either
// a traditional selector chain or a relational Query. Queries are complete
// barriers: CSS selectors on either side of a query segment are never merged
// into the query or into each other.
type queryPlanSegment struct {
	selectors target.Selectors
	query     Query
	isQuery   bool
}

type queryPlan []queryPlanSegment

func (p queryPlan) clone() queryPlan {
	if p == nil {
		return nil
	}
	cloned := make(queryPlan, len(p))
	for i, segment := range p {
		cloned[i] = segment
		cloned[i].selectors = append(target.Selectors(nil), segment.selectors...)
	}
	return cloned
}

func (p queryPlan) appendSelector(selectorType target.Type, value string) queryPlan {
	plan := p.clone()
	if len(plan) > 0 && !plan[len(plan)-1].isQuery {
		last := len(plan) - 1
		plan[last].selectors = plan[last].selectors.Append(selectorType, value)
		return plan
	}
	return append(plan, queryPlanSegment{selectors: target.Selectors{}.Append(selectorType, value)})
}

func (p queryPlan) withSingle() queryPlan {
	plan := p.clone()
	if len(plan) > 0 && !plan[len(plan)-1].isQuery {
		last := len(plan) - 1
		plan[last].selectors = plan[last].selectors.Single()
	}
	return plan
}

func (p queryPlan) withAt(index int) queryPlan {
	plan := p.clone()
	if len(plan) == 0 {
		return plan
	}
	last := &plan[len(plan)-1]
	if last.isQuery {
		last.query = last.query.At(index)
	} else {
		last.selectors = last.selectors.At(index)
	}
	return plan
}

func (p queryPlan) appendQuery(query Query) queryPlan {
	plan := p.clone()
	return append(plan, queryPlanSegment{query: query, isQuery: true})
}

func (p queryPlan) flatSelectors() target.Selectors {
	var selectors target.Selectors
	for _, segment := range p {
		if !segment.isQuery {
			selectors = append(selectors, segment.selectors...)
		}
	}
	return selectors
}

func (p queryPlan) String() string {
	parts := make([]string, 0, len(p))
	for _, segment := range p {
		if segment.isQuery {
			parts = append(parts, fmt.Sprintf("Query(%s)", segment.query))
		} else {
			parts = append(parts, segment.selectors.String())
		}
	}
	return strings.Join(parts, " | ")
}

// planRepository resolves a queryPlan against its session on every terminal
// operation. It deliberately holds no resolved element state, so reusing a
// Selection or Query never reuses stale remote element IDs.
type planRepository struct {
	session apiSession
	plan    queryPlan
}

func newPlanRepository(session apiSession, plan queryPlan) elementRepository {
	return &planRepository{session: session, plan: plan}
}

func (r *planRepository) GetAtLeastOne() ([]element.Element, error) {
	if r == nil {
		return nil, errors.New("empty selection")
	}
	elements, err := r.Get()
	if err != nil {
		return nil, err
	}
	if len(elements) == 0 {
		return nil, errors.New("no elements found")
	}
	return elements, nil
}

func (r *planRepository) GetExactlyOne() (element.Element, error) {
	if r == nil {
		return nil, errors.New("empty selection")
	}
	elements, err := r.GetAtLeastOne()
	if err != nil {
		return nil, err
	}
	if len(elements) > 1 {
		return nil, fmt.Errorf("method does not support multiple elements (%d)", len(elements))
	}
	return elements[0], nil
}

func (r *planRepository) Get() ([]element.Element, error) {
	if r == nil || len(r.plan) == 0 {
		return nil, errors.New("empty selection")
	}

	// Validate every query in the complete expression tree before issuing any
	// remote request. An invalid zero-value query, an empty CSS selector, a
	// negative index, or an invalid nested sub-expression must not cause a
	// partially-executed query.
	for _, segment := range r.plan {
		if segment.isQuery {
			if err := (&segment.query).validate(); err != nil {
				return nil, err
			}
		}
	}

	contexts := []element.Client{r.session}
	var lastElements []element.Element

	for _, segment := range r.plan {
		nextElements := []element.Element{}

		for _, context := range contexts {
			var elements []element.Element
			var err error

			if segment.isQuery {
				elements, err = evalQuery(segment.query, context)
			} else {
				repository := &element.Repository{Client: context, Selectors: segment.selectors}
				elements, err = repository.Get()
			}
			if err != nil {
				return nil, err
			}
			nextElements = append(nextElements, elements...)
		}

		contexts = make([]element.Client, 0, len(nextElements))
		for _, selected := range nextElements {
			contexts = append(contexts, selected)
		}
		lastElements = nextElements
	}

	return lastElements, nil
}
