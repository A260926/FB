//go:build windows

package gotestrunner_test

import "os"

func platformTerminationSignals() []os.Signal {
	return []os.Signal{os.Interrupt}
}
