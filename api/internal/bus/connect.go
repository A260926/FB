package bus

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"net/http"
)

// Connect opens a legacy JSON Wire Protocol session.
func Connect(url string, capabilities map[string]interface{}, httpClient *http.Client) (*Client, error) {
	requestBody, err := capabilitiesToJSON(capabilities)
	if err != nil {
		return nil, err
	}

	if httpClient == nil {
		httpClient = http.DefaultClient
	}

	sessionID, err := openSession(url, requestBody, httpClient)
	if err != nil {
		return nil, err
	}

	sessionURL := fmt.Sprintf("%s/session/%s", url, sessionID)
	return &Client{SessionURL: sessionURL, HTTPClient: httpClient, W3C: false}, nil
}

// ConnectW3C opens a W3C WebDriver session. Capabilities are sent once as
// capabilities.alwaysMatch, with an empty firstMatch array element.
func ConnectW3C(url string, capabilities map[string]interface{}, httpClient *http.Client) (*Client, error) {
	requestBody, err := w3cCapabilitiesToJSON(capabilities)
	if err != nil {
		return nil, err
	}

	if httpClient == nil {
		httpClient = http.DefaultClient
	}

	sessionID, err := openW3CSession(url, requestBody, httpClient)
	if err != nil {
		return nil, err
	}

	sessionURL := fmt.Sprintf("%s/session/%s", url, sessionID)
	return &Client{SessionURL: sessionURL, HTTPClient: httpClient, W3C: true}, nil
}

func capabilitiesToJSON(capabilities map[string]interface{}) (io.Reader, error) {
	if capabilities == nil {
		capabilities = map[string]interface{}{}
	}
	desiredCapabilities := struct {
		DesiredCapabilities map[string]interface{} `json:"desiredCapabilities"`
	}{capabilities}

	capabiltiesJSON, err := json.Marshal(desiredCapabilities)
	if err != nil {
		return nil, err
	}
	return bytes.NewReader(capabiltiesJSON), err
}

// w3cCapabilitiesToJSON converts legacy capability names into their standard
// W3C equivalents. The caller's map (and any nested maps) are never modified.
func w3cCapabilitiesToJSON(capabilities map[string]interface{}) (io.Reader, error) {
	alwaysMatch := map[string]interface{}{}
	for key, value := range capabilities {
		alwaysMatch[key] = copyValue(value)
	}

	if _, present := alwaysMatch["acceptInsecureCerts"]; !present {
		if value, present := alwaysMatch["acceptSslCerts"]; present {
			alwaysMatch["acceptInsecureCerts"] = value
		}
	}
	delete(alwaysMatch, "acceptSslCerts")

	if _, present := alwaysMatch["goog:chromeOptions"]; !present {
		if value, present := alwaysMatch["chromeOptions"]; present {
			alwaysMatch["goog:chromeOptions"] = copyValue(value)
		}
	}
	delete(alwaysMatch, "chromeOptions")

	request := struct {
		Capabilities struct {
			AlwaysMatch map[string]interface{}   `json:"alwaysMatch"`
			FirstMatch  []map[string]interface{} `json:"firstMatch"`
		} `json:"capabilities"`
	}{}
	request.Capabilities.AlwaysMatch = alwaysMatch
	request.Capabilities.FirstMatch = []map[string]interface{}{{}}

	capabilitiesJSON, err := json.Marshal(request)
	if err != nil {
		return nil, err
	}
	return bytes.NewReader(capabilitiesJSON), nil
}

// copyValue performs a shallow copy of nested maps and slices so that the
// request encoder never shares mutable structures with the caller.
func copyValue(value interface{}) interface{} {
	switch typed := value.(type) {
	case map[string]interface{}:
		copied := make(map[string]interface{}, len(typed))
		for key, nested := range typed {
			copied[key] = copyValue(nested)
		}
		return copied
	case []interface{}:
		copied := make([]interface{}, len(typed))
		for index, nested := range typed {
			copied[index] = copyValue(nested)
		}
		return copied
	default:
		return value
	}
}

func openSession(url string, body io.Reader, httpClient *http.Client) (sessionID string, err error) {
	response, responseBody, err := postSession(url, body, httpClient)
	if err != nil {
		return "", err
	}
	httpStatus := 0
	if response != nil {
		httpStatus = response.StatusCode
	}

	var sessionResponse struct {
		Status    *int `json:"status"`
		SessionID string
		// fallback for GeckoDriver
		Value struct {
			SessionID string
		}
	}
	if err := json.Unmarshal(responseBody, &sessionResponse); err != nil {
		return "", err
	}

	if sessionResponse.Status != nil && *sessionResponse.Status != 0 {
		return "", legacyStatusError(httpStatus, responseBody)
	}

	if sessionResponse.SessionID == "" {
		// fallback for GeckoDriver
		if sessionResponse.Value.SessionID != "" {
			return sessionResponse.Value.SessionID, nil
		}
		return "", errors.New("failed to retrieve a session ID")
	}

	return sessionResponse.SessionID, nil
}

func openW3CSession(url string, body io.Reader, httpClient *http.Client) (sessionID string, err error) {
	response, responseBody, err := postSession(url, body, httpClient)
	if err != nil {
		return "", err
	}

	if response.StatusCode < 200 || response.StatusCode > 299 {
		return "", responseError(response.StatusCode, responseBody, true)
	}

	var sessionResponse struct {
		Value struct {
			SessionID    string          `json:"sessionId"`
			Capabilities json.RawMessage `json:"capabilities"`
		} `json:"value"`
	}
	if err := json.Unmarshal(responseBody, &sessionResponse); err != nil {
		return "", invalidW3CSessionError(response.StatusCode, responseBody)
	}

	if sessionResponse.Value.SessionID == "" {
		return "", invalidW3CSessionError(response.StatusCode, responseBody)
	}

	capabilities := bytes.TrimSpace(sessionResponse.Value.Capabilities)
	if len(capabilities) == 0 || capabilities[0] != '{' {
		return "", invalidW3CSessionError(response.StatusCode, responseBody)
	}
	var capabilitiesObject map[string]interface{}
	if err := json.Unmarshal(capabilities, &capabilitiesObject); err != nil {
		return "", invalidW3CSessionError(response.StatusCode, responseBody)
	}

	return sessionResponse.Value.SessionID, nil
}

func invalidW3CSessionError(status int, body []byte) error {
	return &ProtocolError{
		HTTPStatus: status,
		Message:    fmt.Sprintf("invalid session response: HTTP %d: %s", status, responseSummary(body)),
	}
}

func postSession(url string, body io.Reader, httpClient *http.Client) (*http.Response, []byte, error) {
	request, err := http.NewRequest("POST", fmt.Sprintf("%s/session", url), body)
	if err != nil {
		return nil, nil, err
	}

	request.Header.Add("Content-Type", "application/json")

	response, err := httpClient.Do(request)
	if err != nil {
		return nil, nil, err
	}
	defer response.Body.Close()

	responseBody, err := ioutil.ReadAll(response.Body)
	if err != nil {
		return nil, nil, err
	}

	return response, responseBody, nil
}
