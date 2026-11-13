package robotstxt

import (
	"fmt"
	"io"
	"testing"

	"github.com/stretchr/testify/assert"
)

func scanTokens(t *testing.T, input string, chunk int) ([]string, int) {
	t.Helper()
	sc := newStreamScanner(t.Name(), true, &chunkReader{data: []byte(input), chunk: chunk})
	sc.begin()

	var tokens []string
	for {
		tok, ok := sc.nextToken()
		if !ok {
			break
		}
		tokens = append(tokens, tok)
	}
	assert.NoError(t, sc.err())
	return tokens, sc.ErrorCount
}

func TestScanner(t *testing.T) {
	t.Parallel()

	type tcase struct {
		input    string
		expect   []string
		errCount int
	}
	cases := []tcase{
		{"foo", []string{"foo"}, 0},
		{"‐", []string{"‐"}, 0},
		{"# comment \r\nSomething: Somewhere\r\n", []string{tokEOL, "Something", "Somewhere", tokEOL}, 0},
		{"# comment \r\n# more comments\n\nDisallow:\r", []string{tokEOL, tokEOL, "Disallow", tokEOL}, 0},
		{"\xef\xbb\xbfUser-agent: *\n", []string{"User-agent", "*", tokEOL}, 0},
		{"\xd9\xd9", []string{"��"}, 2},
	}
	for i, c := range cases {
		tag := fmt.Sprintf("test-%d", i)
		// chunk 0 means the whole input is delivered in one Read; other
		// values exercise byte-at-a-time and arbitrary split points.
		for _, chunk := range []int{0, 1, 2, 3, 7} {
			t.Run(fmt.Sprintf("%s/chunk-%d", tag, chunk), func(t *testing.T) {
				tokens, errCount := scanTokens(t, c.input, chunk)
				assert.Equal(t, c.errCount, errCount)
				assert.Equal(t, c.expect, tokens)
			})
		}
	}
}

func TestScannerReaderError(t *testing.T) {
	t.Parallel()

	sentinel := io.ErrUnexpectedEOF
	sc := newStreamScanner(t.Name(), true, &errorAfterReader{
		data:  []byte("User-agent: bot\nDisallow: /x\n# tail"),
		err:   sentinel,
		errAt: len("User-agent: bot\nDisallow: /x\n"),
	})
	sc.begin()

	var tokens []string
	for {
		tok, ok := sc.nextToken()
		if !ok {
			break
		}
		tokens = append(tokens, tok)
	}
	assert.Equal(t, sentinel, sc.err())
	assert.NotEmpty(t, tokens)
}
