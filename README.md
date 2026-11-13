# go-expect

![Go](https://github.com/Netflix/go-expect/workflows/Go/badge.svg)
[![Build Status](https://travis-ci.com/Netflix/go-expect.svg?branch=master)](https://travis-ci.com/Netflix/go-expect)
[![GoDoc](https://godoc.org/github.com/Netflix/go-expect?status.svg)](https://godoc.org/github.com/Netflix/go-expect)
[![NetflixOSS Lifecycle](https://img.shields.io/osslifecycle/Netflix/go-expect.svg)]()

Package expect provides an expect-like interface to automate control of applications. It is unlike expect in that it does not spawn or manage process lifecycle. This package only focuses on expecting output and sending input through it's pseudoterminal.

## Usage

### `os.Exec` example

```go
package main

import (
	"log"
	"os"
	"os/exec"
	"time"

	expect "github.com/Netflix/go-expect"
)

func main() {
	c, err := expect.NewConsole(expect.WithStdout(os.Stdout))
	if err != nil {
		log.Fatal(err)
	}
	defer c.Close()

	cmd := exec.Command("vi")
	cmd.Stdin = c.Tty()
	cmd.Stdout = c.Tty()
	cmd.Stderr = c.Tty()

	go func() {
		c.ExpectEOF()
	}()

	err = cmd.Start()
	if err != nil {
		log.Fatal(err)
	}

	time.Sleep(time.Second)
	c.Send("iHello world\x1b")
	time.Sleep(time.Second)
	c.Send("dd")
	time.Sleep(time.Second)
	c.SendLine(":q!")

	err = cmd.Wait()
	if err != nil {
		log.Fatal(err)
	}
}
```

### `golang.org/x/crypto/ssh/terminal` example

```go
package main

import (
	"fmt"

	"golang.org/x/crypto/ssh/terminal"

	expect "github.com/Netflix/go-expect"
)

func getPassword(fd int) string {
	bytePassword, _ := terminal.ReadPassword(fd)

	return string(bytePassword)
}

func main() {
	c, _ := expect.NewConsole()

	defer c.Close()

	donec := make(chan struct{})
	go func() {
		defer close(donec)
		c.SendLine("hunter2")
	}()

	echoText := getPassword(int(c.Tty().Fd()))

	<-donec

	fmt.Printf("\nPassword from stdin: %s", echoText)
}
```

### Compiled expect plans

`Expect` re-runs every matcher against the whole accumulated buffer after
each rune. When you wait repeatedly for the same set of prompt conditions
(for example "all of these status words have appeared"), compile the
conditions once with `CompileExpect` and reuse the resulting `ExpectPlan`
across `ExpectPlan` calls, Consoles and goroutines. Each call keeps its own
matching progress; the plan itself is immutable and safe to share.

```go
package main

import (
	"bytes"
	"log"
	"time"

	expect "github.com/Netflix/go-expect"
)

func main() {
	c, err := expect.NewConsole()
	if err != nil {
		log.Fatal(err)
	}
	defer c.Close()

	// String, Regexp, RegexpPattern, All, Error, EOF, PTSClosed, WithTimeout
	// and Then wrappers are all supported. A custom Matcher cannot be
	// compiled: CompileExpect returns an error and Expect keeps its original
	// per-rune behavior for it.
	plan, err := expect.CompileExpect(
		expect.All(
			expect.String("STATUS=ready"),
			expect.String("phase=2"),
		).Then(func(buf *bytes.Buffer) error {
			log.Printf("prompt complete after %q", buf.String())
			return nil
		}),
		expect.EOF,
		expect.WithTimeout(time.Minute),
	)
	if err != nil {
		log.Fatal(err)
	}

	out, stats, err := c.ExpectPlan(plan)
	log.Printf("matched %q, decoded=%d literalWork=%d signals=%d regexChecks=%d err=%v",
		out, stats.DecodedBytes, stats.LiteralWork,
		stats.LiteralSignals, stats.RegexChecks, err)
}
```

`ExpectPlan` returns `MatchStats` describing the work the call did:

| Counter | Meaning |
| --- | --- |
| `DecodedBytes` | Bytes written into the matched text (`utf8.RuneLen` per rune read, so an invalid byte decoded as `RuneError` is counted exactly as it is buffered). |
| `LiteralWork` | Aho-Corasick transition-table lookups (including every suffix-fallback lookup) plus each output-chain entry examined. |
| `LiteralSignals` | Leaf completion notifications. A declared leaf position is counted once; repeated occurrences of an already matched literal are never re-notified. |
| `RegexChecks` | Number of times a `Regexp`/`RegexpPattern` leaf was evaluated against the accumulated prefix. Leaves inside an already completed `All` are skipped. |

How the work grows for a plan without regexp leaves:

- Compilation is `O(L + N)` where `L` is the total rune length of all
  `String` literals (the shared trie) and `N` the number of condition nodes.
- At runtime every read rune costs one shared automaton transition, an
  amortized `O(1)` table lookup independent of how much text has already
  accumulated. Suffix links let one transition match every literal at once,
  so the cost grows with the decoded text length and `L`, not with text
  length times the number of `String` conditions: there is no
  `strings.Contains` rescan of the whole prefix per condition and per rune.
- A satisfied literal is suppressed on later output chains, so a long output
  full of already matched suffix conditions only performs the per-rune
  transition; `LiteralSignals` never exceeds the number of declared leaf
  positions.
- `All` nodes are only revisited when one of their children completes and
  completion propagates up the dependency edges; they are never walked once
  per rune, including nested `All` nodes whose children satisfy on different
  prefixes.
- Regexp leaves keep the original full-prefix evaluation, run only while the
  leaf can still affect the result. The linearity statement above covers the
  literal machinery; no linear bound is promised for the regexp engines
  themselves.

