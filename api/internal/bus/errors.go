package bus

import (
	"encoding/json"
	"fmt"
	"strings"
)

// ProtocolError describes a failed WebDriver protocol request.
//
// HTTPStatus is the HTTP status code returned by the driver. Code, Message
// and Stacktrace are populated from the error envelope when the driver
// provides one (W3C drivers use value.error, value.message and
// value.stacktrace).
type ProtocolError struct {
	HTTPStatus int
	Code       string
	Message    string
	Stacktrace string
}

func (e *ProtocolError) Error() string {
	message := e.Message
	if e.Code != "" {
		if message != "" {
			message = e.Code + ": " + message
		} else {
			message = e.Code
		}
	}
	if message == "" {
		message = fmt.Sprintf("HTTP %d", e.HTTPStatus)
	}
	return "request unsuccessful: " + message
}

func responseSummary(body []byte) string {
	summary := strings.Join(strings.Fields(string(body)), " ")
	if summary == "" {
		return "<empty response>"
	}
	const maxSummary = 512
	if len(summary) > maxSummary {
		summary = summary[:maxSummary] + "..."
	}
	return summary
}

// responseError parses an unsuccessful HTTP response into a ProtocolError.
func responseError(httpStatus int, body []byte, w3c bool) error {
	if w3c {
		var envelope struct {
			Value struct {
				Error      string `json:"error"`
				Message    string `json:"message"`
				Stacktrace string `json:"stacktrace"`
			} `json:"value"`
		}
		if err := json.Unmarshal(body, &envelope); err != nil || envelope.Value.Error == "" || envelope.Value.Message == "" {
			return &ProtocolError{
				HTTPStatus: httpStatus,
				Message:    fmt.Sprintf("HTTP %d: %s", httpStatus, responseSummary(body)),
			}
		}
		return &ProtocolError{
			HTTPStatus: httpStatus,
			Code:       envelope.Value.Error,
			Message:    envelope.Value.Message,
			Stacktrace: envelope.Value.Stacktrace,
		}
	}

	message := legacyErrorMessage(body)
	if message == "" {
		// Preserve the legacy error text while still exposing the HTTP
		// status and response details on the returned error.
		return &ProtocolError{
			HTTPStatus: httpStatus,
			Message:    responseSummary(body),
		}
	}
	return &ProtocolError{HTTPStatus: httpStatus, Message: message}
}

// legacyErrorMessage extracts the error message from a JSON Wire Protocol
// error envelope. Legacy drivers commonly nest a JSON object containing
// errorMessage inside value.message.
func legacyErrorMessage(body []byte) string {
	var envelope struct {
		Value struct {
			Message json.RawMessage `json:"message"`
		} `json:"value"`
	}
	if err := json.Unmarshal(body, &envelope); err != nil {
		return ""
	}
	return decodeLegacyMessage(envelope.Value.Message)
}

func decodeLegacyMessage(raw json.RawMessage) string {
	if len(raw) == 0 || string(raw) == "null" {
		return ""
	}

	var message string
	if err := json.Unmarshal(raw, &message); err != nil {
		return ""
	}
	message = strings.TrimSpace(message)
	if message == "" {
		return ""
	}

	var nested struct {
		ErrorMessage string `json:"errorMessage"`
	}
	if err := json.Unmarshal([]byte(message), &nested); err == nil && nested.ErrorMessage != "" {
		return nested.ErrorMessage
	}
	return message
}

// legacyStatusError converts a legacy response with a non-zero status into a
// ProtocolError while preserving the error message supplied by the driver.
func legacyStatusError(httpStatus int, body []byte) error {
	message := legacyErrorMessage(body)
	if message == "" {
		message = fmt.Sprintf("HTTP %d: %s", httpStatus, responseSummary(body))
	}
	return &ProtocolError{HTTPStatus: httpStatus, Message: message}
}
