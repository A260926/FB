//go:build !windows

package gotestrunner_test

import (
	"os"
	"syscall"
)

func platformTerminationSignals() []os.Signal {
	return []os.Signal{syscall.SIGTERM}
}
