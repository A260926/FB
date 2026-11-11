// A result guard only. The new behavior and its regression tests are not here.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "TARGET_ERROR:", err)
		os.Exit(1)
	}
}

func run() error {
	root, err := os.Getwd()
	if err != nil {
		return err
	}
	for _, f := range []string{"client_receive_epoch_test.go", "pkg/rtpreceiver/receive_epoch_test.go"} {
		st, e := os.Stat(f)
		if e != nil || !st.Mode().IsRegular() {
			return fmt.Errorf("required new test file missing: %s", f)
		}
	}
	const mod = "github.com/bluenviron/gortsplib/v5"
	required := map[string]bool{}
	for _, test := range []string{"TestClientReceiveEpochTCP", "TestClientReceiveEpochUDP", "TestClientReceiveEpochControl", "TestClientPlayPausePlay", "TestClientPlayPacketNTP", "TestClientPlayRTCPReport", "TestClientPlayDifferentSSRCs"} {
		required[mod+"/"+test] = false
	}
	required[mod+"/pkg/rtpreceiver/TestReceiverEpochReports"] = false
	required[mod+"/pkg/rtpreceiver/TestStandard"] = false
	required[mod+"/pkg/rtpreceiver/TestSequenceNumberOverflow"] = false
	required[mod+"/pkg/rtpreceiver/TestUnrealiableReorder"] = false
	required[mod+"/pkg/rtptime/TestGlobalDecoder"] = false
	names := []string{}
	for key := range required {
		names = append(names, key[strings.LastIndex(key, "/")+1:])
	}
	cmd := exec.Command("go", "test", "-mod=readonly", "-race", "-count=1", "-timeout=90s", "-json", "-run", "^("+strings.Join(names, "|")+")$", ".", "./pkg/rtpreceiver", "./pkg/rtptime")
	cmd.Env = os.Environ()
	for key, value := range map[string]string{"GOTOOLCHAIN": "local", "GOWORK": "off", "GOFLAGS": "-mod=readonly", "GOCACHE": filepath.Join(root, ".author-cache/build"), "GOMODCACHE": filepath.Join(root, ".author-cache/mod"), "GOPROXY": "https://proxy.golang.org", "GOSUMDB": "sum.golang.org"} {
		prefix := key + "="
		filtered := cmd.Env[:0]
		for _, s := range cmd.Env {
			if !strings.HasPrefix(s, prefix) {
				filtered = append(filtered, s)
			}
		}
		cmd.Env = append(filtered, prefix+value)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	cmd.Stderr = os.Stderr
	if err = cmd.Start(); err != nil {
		return err
	}
	scan := bufio.NewScanner(stdout)
	scan.Buffer(make([]byte, 4096), 4*1024*1024)
	var invalid error
	for scan.Scan() {
		var ev struct{ Action, Package, Test, Output string }
		if e := json.Unmarshal(scan.Bytes(), &ev); e != nil {
			invalid = fmt.Errorf("non-JSON go test output: %w", e)
			continue
		}
		if ev.Output != "" {
			fmt.Print(ev.Output)
		}
		if ev.Action == "skip" || ev.Action == "fail" {
			invalid = fmt.Errorf("test %s/%s ended with %s", ev.Package, ev.Test, ev.Action)
		}
		key := ev.Package + "/" + ev.Test
		if ev.Action == "pass" {
			if _, ok := required[key]; ok {
				required[key] = true
			}
		}
	}
	if err = cmd.Wait(); err != nil {
		return err
	}
	if scan.Err() != nil {
		return scan.Err()
	}
	if invalid != nil {
		return invalid
	}
	for name, passed := range required {
		if !passed {
			return fmt.Errorf("required test did not execute and pass: %s", name)
		}
	}
	fmt.Println("TARGET_OK receive epochs and selected existing tests")
	return nil
}
