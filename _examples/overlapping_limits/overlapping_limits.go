package main

import (
	"fmt"

	"github.com/gocolly/colly/v2"
)

func main() {
	c := colly.NewCollector(colly.Async(true))

	// Switch from the default "first matching rule" admission to
	// all-matching admission: every request acquires a slot in each rule
	// matching its host before the download starts.
	c.UseAllMatchingLimits()

	// The wide rule allows three concurrent downloads in total; the
	// site-specific rule additionally caps a.test at a single concurrent
	// download. The outcome is the same regardless of registration order.
	err := c.Limits([]*colly.LimitRule{
		{DomainGlob: "*", Parallelism: 3},
		{DomainGlob: "a.test", Parallelism: 1},
	})
	if err != nil {
		panic(err)
	}

	c.OnRequest(func(r *colly.Request) {
		fmt.Println("visiting", r.URL)
	})

	for _, u := range []string{
		"http://a.test/1",
		"http://a.test/2",
		"http://b.test/1",
		"http://b.test/2",
		"http://b.test/3",
	} {
		if err := c.Visit(u); err != nil {
			fmt.Println("visit error:", err)
		}
	}

	c.Wait()
}
