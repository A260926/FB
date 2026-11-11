// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package nack

import (
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/logging"
)

// GeneratorOption can be used to configure GeneratorInterceptor.
type GeneratorOption func(r *GeneratorInterceptor) error

// GeneratorSize sets the size of the interceptor.
// Size must be one of: 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768.
func GeneratorSize(size uint16) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.size = size

		return nil
	}
}

// GeneratorSkipLastN sets the number of packets (n-1 packets before the last received packets)
//
//	to ignore when generating nack requests.
func GeneratorSkipLastN(skipLastN uint16) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.skipLastN = skipLastN

		return nil
	}
}

// GeneratorMaxNacksPerPacket sets the maximum number of NACKs sent per missing
// packet. Only batches whose RTCP writer call succeeds (Write returns nil)
// count against this limit: a failed write leaves the budget untouched and the
// missing packet is retried in the next feedback cycle. If set to 0 (the
// default), the number of NACKs per missing packet is unlimited. The counter
// is recycled once the missing packet is repaired, leaves the retained window
// or the stream is unbound, so a sequence number that re-appears after a full
// 16-bit wrap starts with a fresh budget.
func GeneratorMaxNacksPerPacket(maxNacks uint16) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.maxNacksPerPacket = maxNacks

		return nil
	}
}

// GeneratorLog sets a logger for the interceptor.
func GeneratorLog(log logging.LeveledLogger) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.log = log

		return nil
	}
}

// WithGeneratorLoggerFactory sets a logger factory for the interceptor.
func WithGeneratorLoggerFactory(loggerFactory logging.LoggerFactory) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.loggerFactory = loggerFactory

		return nil
	}
}

// GeneratorInterval sets the nack send interval for the interceptor.
func GeneratorInterval(interval time.Duration) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.interval = interval

		return nil
	}
}

// GeneratorStreamsFilter sets filter for generator streams.
func GeneratorStreamsFilter(filter func(info *interceptor.StreamInfo) bool) GeneratorOption {
	return func(r *GeneratorInterceptor) error {
		r.streamsFilter = filter

		return nil
	}
}
