package aiff
import "testing"
// taskCheck is a countable assertion used by the explicitly named new tests.
func taskCheck(t *testing.T, ok bool, message string) {
 t.Helper()
 if !ok { t.Fatal(message) }
 t.Log("CHECK_PASS "+t.Name())
}
