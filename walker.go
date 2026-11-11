package ftp

import (
	"net/textproto"
	"path"
)

// Walker traverses the directory tree of a remote FTP server
type Walker struct {
	serverConn *ServerConn
	root       string
	cur        *item
	stack      []*item
	descend    bool
	onError    func(path string, err error) bool
	done       bool
}

type item struct {
	path  string
	entry *Entry
	err   error
}

// WalkOptions configures the behavior of a Walker created with
// [ServerConn.WalkWithOptions].
type WalkOptions struct {
	// OnError is called when listing a directory fails, after the data
	// connection (if any) and the control connection reply have both been
	// fully processed, so it is safe to issue other commands (such as
	// [ServerConn.NoOp]) from the callback.
	//
	// The callback receives the full path of the failed directory and the
	// original, non-stringified error, which can be inspected with errors.As.
	//
	// Returning true skips the failed directory and continues the walk with
	// the already queued siblings. Only a plain [textproto.Error] with status
	// code 550 can be skipped this way: any other failure (for example a 421
	// reply, a broken connection or a data transfer error) stops the walk
	// even if the callback returns true, and a 550 joined with another error
	// is not skippable either.
	//
	// Returning false stops the walk and makes [Walker.Err] return the error.
	// The callback is invoked at most once for a given failed directory.
	OnError func(path string, err error) bool
}

// isSkippableBranchError reports whether err is a plain textproto.Error
// carrying a 550 status. A joined error, for instance a 550 accompanied by a
// data connection read or closing error, is not skippable even though
// errors.As could find the 550 inside it.
func isSkippableBranchError(err error) bool {
	tpErr, ok := err.(*textproto.Error)
	return ok && tpErr.Code == StatusFileUnavailable
}

// Next advances the Walker to the next file or directory,
// which will then be available through the Path, Stat, and Err methods.
// It returns false when the walk stops at the end of the tree.
func (w *Walker) Next() bool {
	// Once the walk is finished, or stopped by an error, further calls must
	// not send any command or notify the callback again.
	if w.done {
		return false
	}

	// check if we need to init cur, maybe this should be inside Walk
	if w.cur == nil {
		w.cur = &item{
			path: w.root,
			entry: &Entry{
				Type: EntryTypeFolder,
			},
		}
	}

	if w.descend && w.cur.entry.Type == EntryTypeFolder {
		entries, err := w.serverConn.List(w.cur.path)

		// an error occurred, drop out and stop walking
		if err != nil {
			w.cur.err = err

			if w.onError != nil && isSkippableBranchError(err) && w.onError(w.cur.path, err) {
				// The caller accepts to skip this branch: drop the error and
				// keep processing the siblings already queued in the stack.
				w.cur.err = nil
			} else {
				w.done = true
				return false
			}
		}

		for _, entry := range entries {
			if entry.Name == "." || entry.Name == ".." {
				continue
			}

			item := &item{
				path:  path.Join(w.cur.path, entry.Name),
				entry: entry,
			}

			w.stack = append(w.stack, item)
		}
	}

	if len(w.stack) == 0 {
		w.done = true
		return false
	}

	// update cur
	i := len(w.stack) - 1
	w.cur = w.stack[i]
	w.stack = w.stack[:i]

	// reset SkipDir
	w.descend = true

	return true
}

// SkipDir tells the Next function to skip the currently processed directory
func (w *Walker) SkipDir() {
	w.descend = false
}

// Err returns the error, if any, for the most recent attempt by Next to
// visit a file or a directory. If a directory has an error, the walker
// will not descend in that directory
func (w *Walker) Err() error {
	return w.cur.err
}

// Stat returns info for the most recent file or directory
// visited by a call to Next.
func (w *Walker) Stat() *Entry {
	return w.cur.entry
}

// Path returns the path to the most recent file or directory
// visited by a call to Next. It contains the argument to Walk
// as a prefix; that is, if Walk is called with "dir", which is
// a directory containing the file "a", Path will return "dir/a".
func (w *Walker) Path() string {
	return w.cur.path
}
