// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package jitterbuffer

import (
	"time"

	"github.com/pion/logging"
)

// ReceiverInterceptorOption can be used to configure ReceiverInterceptor.
type ReceiverInterceptorOption func(d *ReceiverInterceptor) error

// Log sets a logger for the interceptor.
func Log(log logging.LeveledLogger) ReceiverInterceptorOption {
	return func(d *ReceiverInterceptor) error {
		d.log = log

		return nil
	}
}

// WithLoggerFactory sets a logger factory for the interceptor.
func WithLoggerFactory(loggerFactory logging.LoggerFactory) ReceiverInterceptorOption {
	return func(d *ReceiverInterceptor) error {
		d.loggerFactory = loggerFactory

		return nil
	}
}

// WithTimedPlayout switches the interceptor to RTP media-clock scheduling.
// A separate timed JitterBuffer is created per bound SSRC using that stream's
// StreamInfo.ClockRate, so multiple streams can run at different sample rates.
// The caller supplies the clock and must keep it monotonic; no goroutines or
// real timers are created. Returns ErrInvalidPlayoutConfig for a negative
// delay or a nil clock.
func WithTimedPlayout(delay time.Duration, now func() time.Time) ReceiverInterceptorOption {
	return func(d *ReceiverInterceptor) error {
		if delay < 0 || now == nil {
			return ErrInvalidPlayoutConfig
		}
		d.timed = true
		d.timedDelay = delay
		d.timedNow = now

		return nil
	}
}
