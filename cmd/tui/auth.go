package main

import (
	"fmt"
	"net/http"
	"strings"

	"github.com/robert-malhotra/go-stac-client/pkg/client"
)

type authMode string

const (
	authModeNone   authMode = "none"
	authModeBearer authMode = "bearer"
	authModeBasic  authMode = "basic"
	authModeHeader authMode = "header"
)

type authConfig struct {
	mode        authMode
	token       string
	username    string
	password    string
	headerName  string
	headerValue string
}

func (cfg authConfig) validate() error {
	switch cfg.mode {
	case authModeNone:
		return nil
	case authModeBearer:
		if strings.TrimSpace(cfg.token) == "" {
			return fmt.Errorf("Bearer token is required")
		}
	case authModeBasic:
		if strings.TrimSpace(cfg.username) == "" {
			return fmt.Errorf("Username is required for basic authentication")
		}
	case authModeHeader:
		if strings.TrimSpace(cfg.headerName) == "" {
			return fmt.Errorf("Header name is required")
		}
		if strings.TrimSpace(cfg.headerValue) == "" {
			return fmt.Errorf("Header value is required")
		}
	default:
		return fmt.Errorf("Unsupported authentication mode: %s", cfg.mode)
	}
	return nil
}

func (cfg authConfig) middleware() (client.Middleware, error) {
	switch cfg.mode {
	case authModeNone:
		return nil, nil
	case authModeBearer:
		token := strings.TrimSpace(cfg.token)
		if token == "" {
			return nil, fmt.Errorf("Bearer token is required")
		}
		return func(r *http.Request) error {
			r.Header.Set("Authorization", "Bearer "+token)
			return nil
		}, nil
	case authModeBasic:
		username := strings.TrimSpace(cfg.username)
		if username == "" {
			return nil, fmt.Errorf("Username is required for basic authentication")
		}
		password := cfg.password
		return func(r *http.Request) error {
			r.SetBasicAuth(username, password)
			return nil
		}, nil
	case authModeHeader:
		name := strings.TrimSpace(cfg.headerName)
		if name == "" {
			return nil, fmt.Errorf("Header name is required")
		}
		value := cfg.headerValue
		if strings.TrimSpace(value) == "" {
			return nil, fmt.Errorf("Header value is required")
		}
		canonical := http.CanonicalHeaderKey(name)
		return func(r *http.Request) error {
			r.Header.Set(canonical, value)
			return nil
		}, nil
	default:
		return nil, fmt.Errorf("unsupported authentication mode: %s", cfg.mode)
	}
}

func (cfg authConfig) equal(other authConfig) bool {
	if cfg.mode != other.mode {
		return false
	}
	switch cfg.mode {
	case authModeBearer:
		return cfg.token == other.token
	case authModeBasic:
		return cfg.username == other.username && cfg.password == other.password
	case authModeHeader:
		return cfg.headerName == other.headerName && cfg.headerValue == other.headerValue
	default:
		return true
	}
}
