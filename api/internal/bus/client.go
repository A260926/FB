package bus

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/ioutil"
	"net/http"
	"strings"
)

type Client struct {
	SessionURL string
	HTTPClient *http.Client
	// W3C reports whether this session uses the W3C WebDriver protocol.
	W3C bool
}

// IsW3C reports whether this client speaks the W3C WebDriver protocol.
func (c *Client) IsW3C() bool {
	return c.W3C
}

func (c *Client) Send(method, endpoint string, body interface{}, result interface{}) error {
	requestBody, err := bodyToJSON(body)
	if err != nil {
		return err
	}

	requestURL := strings.TrimSuffix(c.SessionURL+"/"+endpoint, "/")
	responseBody, statusCode, err := c.makeRequest(requestURL, method, requestBody)
	if err != nil {
		return err
	}

	if err := c.decodeResponse(statusCode, responseBody, result); err != nil {
		return err
	}

	return nil
}

func bodyToJSON(body interface{}) ([]byte, error) {
	if body == nil {
		return nil, nil
	}
	bodyJSON, err := json.Marshal(body)
	if err != nil {
		return nil, fmt.Errorf("invalid request body: %s", err)
	}
	return bodyJSON, nil
}

func (c *Client) makeRequest(url, method string, body []byte) ([]byte, int, error) {
	request, err := http.NewRequest(method, url, bytes.NewReader(body))
	if err != nil {
		return nil, 0, fmt.Errorf("invalid request: %s", err)
	}

	if body != nil {
		request.Header.Add("Content-Type", "application/json")
	}

	response, err := c.HTTPClient.Do(request)
	if err != nil {
		return nil, 0, fmt.Errorf("request failed: %s", err)
	}
	defer response.Body.Close()

	responseBody, err := ioutil.ReadAll(response.Body)
	if err != nil {
		return nil, response.StatusCode, err
	}

	if response.StatusCode < 200 || response.StatusCode > 299 {
		return nil, response.StatusCode, responseError(response.StatusCode, responseBody, c.W3C)
	}

	return responseBody, response.StatusCode, nil
}

// decodeResponse validates the protocol envelope and unmarshals value into
// result. In the legacy dialect a non-zero status is a failure even when the
// HTTP status is successful. In the W3C dialect a successful HTTP response is
// always treated as success; a script result that happens to contain an
// "error" field is therefore not mistaken for a protocol failure.
func (c *Client) decodeResponse(httpStatus int, responseBody []byte, result interface{}) error {
	if c.W3C {
		if result == nil {
			return nil
		}
		bodyValue := struct {
			Value *json.RawMessage `json:"value"`
		}{}
		if err := json.Unmarshal(responseBody, &bodyValue); err != nil || bodyValue.Value == nil {
			return &ProtocolError{
				HTTPStatus: httpStatus,
				Message:    fmt.Sprintf("invalid response: HTTP %d: %s", httpStatus, responseSummary(responseBody)),
			}
		}
		if err := json.Unmarshal(*bodyValue.Value, result); err != nil {
			return fmt.Errorf("unexpected response: %s", responseBody)
		}
		return nil
	}

	var legacyEnvelope struct {
		Status *int `json:"status"`
	}
	if err := json.Unmarshal(responseBody, &legacyEnvelope); err == nil &&
		legacyEnvelope.Status != nil && *legacyEnvelope.Status != 0 {
		return legacyStatusError(httpStatus, responseBody)
	}

	if result != nil {
		bodyValue := struct{ Value interface{} }{result}
		if err := json.Unmarshal(responseBody, &bodyValue); err != nil {
			return fmt.Errorf("unexpected response: %s", responseBody)
		}
	}

	return nil
}
