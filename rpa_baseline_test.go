package webdriver

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// Explicit deterministic geometry only: does not calculate edges or assignment.
func TestRPABaseline(t *testing.T) {
	debug = false
	rectangles := map[string][4]int{"l1": {0, 0, 10, 20}, "l2": {0, 10, 10, 20}, "c1": {11, 5, 10, 20}, "c2": {15, 0, 10, 10}}
	sent := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var value interface{}
		switch {
		case r.URL.Path == "/session":
			value = map[string]interface{}{}
		case r.URL.Path == "/session/s/element":
			value = map[string]string{"ELEMENT": "card"}
		case r.URL.Path == "/session/s/element/card/elements":
			var q map[string]interface{}
			json.NewDecoder(r.Body).Decode(&q)
			if q["value"] == "label" {
				value = []map[string]string{{"ELEMENT": "l1"}, {"ELEMENT": "l2"}}
			} else {
				value = []map[string]string{{"ELEMENT": "c1"}, {"ELEMENT": "c2"}}
			}
		case strings.HasSuffix(r.URL.Path, "/value"):
			var q struct{ Value []string }
			json.NewDecoder(r.Body).Decode(&q)
			sent = strings.Join(q.Value, "") == "42"
			value = nil
		default:
			parts := strings.Split(r.URL.Path, "/")
			if len(parts) != 6 {
				t.Errorf("unexpected original path %s", r.URL.Path)
				break
			}
			a, ok := rectangles[parts[4]]
			if !ok {
				t.Errorf("unknown element %s", parts[4])
				break
			}
			switch parts[5] {
			case "location":
				value = map[string]int{"x": a[0], "y": a[1]}
			case "size":
				value = map[string]int{"width": a[2], "height": a[3]}
			default:
				t.Errorf("unexpected original geometry endpoint %s", r.URL.Path)
			}
		}
		json.NewEncoder(w).Encode(map[string]interface{}{"status": 0, "sessionId": "s", "value": value})
	}))
	defer server.Close()
	d := &ChromeDriver{WebDriverCore: WebDriverCore{url: server.URL}}
	s, err := d.NewSession(nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	card, err := s.FindElement(ID, "card")
	if err != nil {
		t.Fatal(err)
	}
	labels, err := card.FindElements(TagName, "label")
	if err != nil {
		t.Fatal(err)
	}
	controls, err := card.FindElements(TagName, "input")
	if err != nil {
		t.Fatal(err)
	}
	if len(labels) != 2 || len(controls) != 2 {
		t.Fatal("original lookup failed")
	}
	for _, e := range append(labels, controls...) {
		pos, err := e.GetLocation()
		if err != nil {
			t.Fatal(err)
		}
		size, err := e.Size()
		if err != nil {
			t.Fatal(err)
		}
		fmt.Printf("RECT %s %d %d %d %d\n", e.id, pos.X, pos.Y, size.Width, size.Height)
	}
	if err = controls[0].SendKeys("42"); err != nil {
		t.Fatal(err)
	}
	if !sent {
		t.Fatal("original input command failed")
	}
	fmt.Println("BASELINE original scoped labels=2 controls=2 sendkeys=42")
}
