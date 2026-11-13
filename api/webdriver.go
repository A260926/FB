package api

import (
	"fmt"
	"net/http"
	"time"

	"github.com/sclevine/agouti/api/internal/service"
)

type WebDriver struct {
	Timeout    time.Duration
	Debug      bool
	HTTPClient *http.Client
	service    driverService
	sessions   []*Session
}

type driverService interface {
	URL() string
	Start(debug bool) error
	Stop() error
	WaitForBoot(timeout time.Duration) error
}

func NewWebDriver(url string, command []string) *WebDriver {
	driverService := &service.Service{
		URLTemplate: url,
		CmdTemplate: command,
	}

	return &WebDriver{
		Timeout: 10 * time.Second,
		service: driverService,
	}
}

func (w *WebDriver) URL() string {
	return w.service.URL()
}

func (w *WebDriver) Open(desiredCapabilites map[string]interface{}) (*Session, error) {
	return w.open(desiredCapabilites, false)
}

// OpenW3C opens a W3C WebDriver session using the driver service URL.
func (w *WebDriver) OpenW3C(capabilities map[string]interface{}) (*Session, error) {
	return w.open(capabilities, true)
}

func (w *WebDriver) open(capabilities map[string]interface{}, w3c bool) (*Session, error) {
	url := w.service.URL()
	if url == "" {
		return nil, fmt.Errorf("service not started")
	}

	var (
		session *Session
		err     error
	)
	if w3c {
		session, err = OpenW3CWithClient(url, capabilities, w.HTTPClient)
	} else {
		session, err = OpenWithClient(url, capabilities, w.HTTPClient)
	}
	if err != nil {
		return nil, err
	}

	w.sessions = append(w.sessions, session)
	return session, nil
}

func (w *WebDriver) Start() error {
	if err := w.service.Start(w.Debug); err != nil {
		return fmt.Errorf("failed to start service: %s", err)
	}

	if err := w.service.WaitForBoot(w.Timeout); err != nil {
		w.service.Stop()
		return err
	}

	return nil
}

func (w *WebDriver) Stop() error {
	for _, session := range w.sessions {
		session.Delete()
	}

	if err := w.service.Stop(); err != nil {
		return fmt.Errorf("failed to stop service: %s", err)
	}

	return nil
}
