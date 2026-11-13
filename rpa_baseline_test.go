package godet

import (
	"encoding/json"
	"fmt"
	"github.com/gorilla/websocket"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// Protocol fixture only: no AX projection, semantic query or backend conversion.
func rpaConnect(t *testing.T, reply func(Params) Params) *RemoteDebugger {
	t.Helper()
	mux := http.NewServeMux()
	server := httptest.NewServer(mux)
	mux.HandleFunc("/json/list", func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode([]map[string]interface{}{{"id": "page1", "type": "page", "webSocketDebuggerUrl": "ws" + strings.TrimPrefix(server.URL, "http") + "/devtools/page/page1"}})
	})
	mux.HandleFunc("/devtools/page/page1", func(w http.ResponseWriter, r *http.Request) {
		ws, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer ws.Close()
		for {
			var q Params
			if ws.ReadJSON(&q) != nil {
				return
			}
			a := reply(q)
			a["id"] = q["id"]
			if ws.WriteJSON(a) != nil {
				return
			}
		}
	})
	remote, err := Connect(strings.TrimPrefix(server.URL, "http://"), false)
	if err != nil {
		server.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() { remote.Close(); server.Close() })
	return remote
}

func TestRPABaseline(t *testing.T) {
	focused := false
	remote := rpaConnect(t, func(q Params) Params {
		p, _ := q["params"].(map[string]interface{})
		switch q["method"] {
		case "DOM.getDocument":
			return Params{"result": Params{"root": Params{"nodeId": 1}}}
		case "DOM.querySelector":
			return Params{"result": Params{"nodeId": 11}}
		case "DOM.focus":
			focused = p["nodeId"] == float64(11)
			return Params{"result": Params{}}
		case "Accessibility.getFullAXTree":
			return Params{"result": Params{"nodes": []Params{
				{"nodeId": "r", "ignored": false, "role": Params{"type": "internalRole", "value": "RootWebArea"}, "childIds": []string{"g"}},
				{"nodeId": "g", "parentId": "r", "ignored": true, "childIds": []string{"f"}},
				{"nodeId": "f", "parentId": "g", "ignored": false, "role": Params{"type": "role", "value": "form"}, "name": Params{"type": "computedString", "value": "Invoice"}, "childIds": []string{"b"}},
				{"nodeId": "b", "parentId": "f", "ignored": false, "backendDOMNodeId": 111, "role": Params{"type": "role", "value": "textbox"}, "name": Params{"type": "computedString", "value": "Amount"}},
			}}}
		default:
			t.Errorf("unexpected original command: %v", q["method"])
			return Params{"result": Params{}}
		}
	})
	doc, err := remote.GetDocument()
	if err != nil {
		t.Fatal(err)
	}
	root := int(doc["root"].(map[string]interface{})["nodeId"].(float64))
	found, err := remote.QuerySelector(root, "#amount")
	if err != nil {
		t.Fatal(err)
	}
	if err = remote.Focus(int(found["nodeId"].(float64))); err != nil {
		t.Fatal(err)
	}
	ax, err := remote.SendRequest("Accessibility.getFullAXTree", nil)
	if err != nil {
		t.Fatal(err)
	}
	nodes := ax["nodes"].([]interface{})
	if len(nodes) != 4 || !focused {
		t.Fatal("original command path failed")
	}
	fmt.Println("BASELINE original DOM.focus node=11 AX.nodes=4")
}
