package live

import (
	"testing"

	"github.com/datarhei/gosrt/circular"
	"github.com/datarhei/gosrt/packet"
	"github.com/stretchr/testify/require"
)

// bwDelivery records a packet delivered by the sender under test.
type bwDelivery struct {
	now       uint64
	seq       uint32
	retrans   bool
	timestamp uint32
	payload   int
}

// bwSender is a sender in absolute bandwidth mode with a deterministic clock.
type bwSender struct {
	sender     *sender
	now        uint64
	deliveries []bwDelivery
}

func newBWSender(initialSeq uint32, maxBW int64, dropThreshold uint64) *bwSender {
	b := &bwSender{}

	b.sender = NewSender(SendConfig{
		InitialSequenceNumber: circular.New(initialSeq, packet.MAX_SEQUENCENUMBER),
		DropThreshold:         dropThreshold,
		MaxBW:                 maxBW,
		OnDeliver: func(p packet.Packet) {
			b.deliveries = append(b.deliveries, bwDelivery{
				now:       b.now,
				seq:       p.Header().PacketSequenceNumber.Val(),
				retrans:   p.Header().RetransmittedPacketFlag,
				timestamp: p.Header().Timestamp,
				payload:   int(p.Len()),
			})
		},
	}).(*sender)

	return b
}

func (b *bwSender) push(size int, tsbpdTime uint64) {
	p := packet.NewPacket(nil)
	p.SetData(make([]byte, size))
	p.Header().PktTsbpdTime = tsbpdTime
	b.sender.Push(p)
}

func (b *bwSender) tick(now uint64) {
	b.now = now
	b.sender.Tick(now)
}

func (b *bwSender) nak(first, last uint32) {
	b.sender.NAK([]circular.Number{
		circular.New(first, packet.MAX_SEQUENCENUMBER),
		circular.New(last, packet.MAX_SEQUENCENUMBER),
	})
}

func (b *bwSender) ack(seq uint32) {
	b.sender.ACK(circular.New(seq, packet.MAX_SEQUENCENUMBER))
}

func (b *bwSender) requireDeliveries(t *testing.T, want []bwDelivery) {
	t.Helper()
	require.Equal(t, want, b.deliveries)
}

// TestAbsoluteBandwidth verifies the absolute bandwidth mode of the live
// sender that is active for a positive MaxBW.
func TestAbsoluteBandwidth(t *testing.T) {
	t.Run("Pacing", func(t *testing.T) {
		// 84 bytes payload + 16 bytes SRT header = 100 bytes per packet.
		// At 1000 bytes/s this is one packet every 100000 microseconds.
		b := newBWSender(0, 1000, 10_000_000)

		for range 3 {
			b.push(84, 0)
		}

		b.tick(0)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
		})

		stats := b.sender.Stats()
		require.Equal(t, uint64(1), stats.PktUnique)
		require.Equal(t, uint64(3), stats.PktBuf)
		require.Equal(t, uint64(3*84), stats.ByteBuf)
		require.Equal(t, float64(100000), stats.UsPktSndPeriod)

		// Not enough time has passed for the next packet
		b.tick(99999)
		require.Len(t, b.deliveries, 1)

		b.tick(100000)
		require.Len(t, b.deliveries, 2)
		require.Equal(t, bwDelivery{now: 100000, seq: 1, retrans: false, timestamp: 0, payload: 84}, b.deliveries[1])

		b.tick(199999)
		require.Len(t, b.deliveries, 2)

		b.tick(200000)
		require.Len(t, b.deliveries, 3)
		require.Equal(t, bwDelivery{now: 200000, seq: 2, retrans: false, timestamp: 0, payload: 84}, b.deliveries[2])

		// The queue is empty, nothing more can be delivered
		b.tick(300000)
		require.Len(t, b.deliveries, 3)

		stats = b.sender.Stats()
		require.Equal(t, uint64(3), stats.PktUnique)
		require.Equal(t, uint64(0), stats.PktRetrans)
		require.Equal(t, uint64(3), stats.PktBuf)
		require.Equal(t, uint64(3*84), stats.ByteBuf)
	})

	t.Run("RetransmissionAndACK", func(t *testing.T) {
		b := newBWSender(0, 1000, 10_000_000)

		for range 3 {
			b.push(84, 0)
		}

		b.tick(0)
		b.tick(100000)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
			{now: 100000, seq: 1, retrans: false, timestamp: 0, payload: 84},
		})

		// A NAK for a packet that has not been sent yet must not turn
		// the still queued original packet into a retransmission.
		b.nak(2, 2)

		// The first packet gets lost, the retransmission is paced
		b.nak(0, 0)
		b.tick(150000)
		require.Len(t, b.deliveries, 2)

		b.tick(200000)
		require.Len(t, b.deliveries, 3)
		require.Equal(t, bwDelivery{now: 200000, seq: 0, retrans: true, timestamp: 0, payload: 84}, b.deliveries[2])

		// A new NAK after a successful retransmission queues the
		// packet again.
		b.nak(0, 0)
		b.tick(300000)
		require.Len(t, b.deliveries, 4)
		require.Equal(t, bwDelivery{now: 300000, seq: 0, retrans: true, timestamp: 0, payload: 84}, b.deliveries[3])

		// Overlapping NAKs for a packet that is already waiting for
		// retransmission must not queue it more than once.
		b.nak(1, 1)
		b.nak(1, 1)
		b.nak(1, 1)

		// The ACK covers the packet before its retransmission slot,
		// the pending retransmission request is cancelled and the
		// still valid original packet is sent instead.
		b.ack(2)
		b.tick(400000)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
			{now: 100000, seq: 1, retrans: false, timestamp: 0, payload: 84},
			{now: 200000, seq: 0, retrans: true, timestamp: 0, payload: 84},
			{now: 300000, seq: 0, retrans: true, timestamp: 0, payload: 84},
			{now: 400000, seq: 2, retrans: false, timestamp: 0, payload: 84},
		})

		stats := b.sender.Stats()
		require.Equal(t, uint64(3), stats.PktUnique)
		require.Equal(t, uint64(2), stats.PktRetrans)
		require.Equal(t, uint64(1), stats.PktBuf)
		require.Equal(t, uint64(84), stats.ByteBuf)

		b.ack(3)
		stats = b.sender.Stats()
		require.Equal(t, uint64(0), stats.PktBuf)
		require.Equal(t, uint64(0), stats.ByteBuf)

		b.tick(500000)
		require.Len(t, b.deliveries, 5)
	})

	t.Run("DropThresholdBoundary", func(t *testing.T) {
		// An original packet that reaches its TSBPD time plus the drop
		// threshold while waiting must not be delivered anymore.
		b := newBWSender(0, 1000, 50_000)
		b.push(84, 0)

		b.tick(60_000)
		require.Empty(t, b.deliveries)

		stats := b.sender.Stats()
		require.Equal(t, uint64(1), stats.PktDrop)
		require.Equal(t, uint64(0), stats.PktBuf)
		require.Equal(t, uint64(0), stats.ByteBuf)

		// Just before the boundary the packet is still valid.
		b = newBWSender(0, 1000, 50_000)
		b.push(84, 0)

		b.tick(49_999)
		b.requireDeliveries(t, []bwDelivery{
			{now: 49_999, seq: 0, retrans: false, timestamp: 0, payload: 84},
		})

		// A packet waiting for retransmission is dropped at the
		// boundary as well, as is a still waiting original packet.
		b = newBWSender(0, 1000, 50_000)
		b.push(84, 0)
		b.push(84, 0)

		b.tick(0)
		require.Len(t, b.deliveries, 1)

		b.nak(0, 0)
		b.tick(50_000)
		require.Len(t, b.deliveries, 1)

		stats = b.sender.Stats()
		require.Equal(t, uint64(2), stats.PktDrop)
		require.Equal(t, uint64(0), stats.PktBuf)
		require.Equal(t, uint64(0), stats.ByteBuf)

		// A late ACK or NAK must not touch the released packets.
		b.ack(2)
		b.nak(0, 1)
		stats = b.sender.Stats()
		require.Equal(t, uint64(2), stats.PktDrop)
		require.Equal(t, uint64(0), stats.PktBuf)
		require.Equal(t, uint64(0), stats.ByteBuf)

		b.tick(100_000)
		require.Len(t, b.deliveries, 1)
	})

	t.Run("IdleRecovery", func(t *testing.T) {
		b := newBWSender(0, 1000, 10_000_000)

		b.push(84, 0)
		b.tick(0)
		require.Len(t, b.deliveries, 1)

		// Long idle time without any data. The send opportunities
		// during this time must not accumulate.
		b.tick(500_000)
		b.tick(1_000_000)
		require.Len(t, b.deliveries, 1)

		// The first packet after the idle time is sent immediately,
		// the following ones are paced again. No burst.
		b.push(84, 1_000_000)
		b.push(84, 1_000_000)
		b.push(84, 1_000_000)

		b.tick(1_000_001)
		require.Len(t, b.deliveries, 2)
		require.Equal(t, bwDelivery{now: 1_000_001, seq: 1, retrans: false, timestamp: 1_000_000, payload: 84}, b.deliveries[1])

		b.tick(1_000_002)
		b.tick(1_099_999)
		require.Len(t, b.deliveries, 2)

		b.tick(1_100_000)
		require.Len(t, b.deliveries, 3)
		require.Equal(t, bwDelivery{now: 1_100_000, seq: 2, retrans: false, timestamp: 1_000_000, payload: 84}, b.deliveries[2])

		b.tick(1_200_000)
		require.Len(t, b.deliveries, 4)

		// A packet whose TSBPD time is in the future must not be
		// sent early.
		b.push(84, 2_000_000)
		b.tick(1_500_000)
		b.tick(1_999_999)
		require.Len(t, b.deliveries, 4)

		b.tick(2_000_000)
		require.Len(t, b.deliveries, 5)
		require.Equal(t, bwDelivery{now: 2_000_000, seq: 4, retrans: false, timestamp: 2_000_000, payload: 84}, b.deliveries[4])
	})

	t.Run("FractionalPeriod", func(t *testing.T) {
		// 100 bytes per packet at 3000 bytes/s is one packet every
		// 33333.333... microseconds. The fractional part must be kept,
		// otherwise the error accumulates linearly over the packets.
		b := newBWSender(0, 3000, 100_000_000)

		const n = 300
		for range n {
			b.push(84, 0)
		}

		for k := uint64(0); k < n; k++ {
			// earliest delivery time of packet k is k * 100000 / 3
			exact := k * 100000 / 3
			if k*100000%3 != 0 {
				exact++
			}

			if exact > 0 {
				b.tick(exact - 1)
				require.Len(t, b.deliveries, int(k), "packet %d must not be delivered before %d", k, exact)
			}

			b.tick(exact)
			require.Len(t, b.deliveries, int(k)+1, "packet %d must be delivered at %d", k, exact)
			require.Equal(t, exact, b.deliveries[k].now)
		}

		// With a truncated period the last packet would have been
		// delivered at 299*33333 = 9966567, about 100 microseconds
		// too early.
		require.Equal(t, uint64(9966667), b.deliveries[n-1].now)

		stats := b.sender.Stats()
		require.Equal(t, uint64(n), stats.PktUnique)
	})

	t.Run("MixedPacketSizes", func(t *testing.T) {
		// Each packet is charged with its own size plus 16 bytes of
		// SRT header.
		b := newBWSender(0, 1000, 10_000_000)

		b.push(84, 0)  // 100 bytes -> 100000 us
		b.push(500, 0) // 516 bytes -> 516000 us
		b.push(84, 0)
		b.push(500, 0)

		b.tick(0)
		b.tick(99_999)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
		})

		b.tick(100_000)
		b.tick(615_999)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
			{now: 100_000, seq: 1, retrans: false, timestamp: 0, payload: 500},
		})

		b.tick(616_000)
		b.tick(715_999)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
			{now: 100_000, seq: 1, retrans: false, timestamp: 0, payload: 500},
			{now: 616_000, seq: 2, retrans: false, timestamp: 0, payload: 84},
		})

		b.tick(716_000)
		b.requireDeliveries(t, []bwDelivery{
			{now: 0, seq: 0, retrans: false, timestamp: 0, payload: 84},
			{now: 100_000, seq: 1, retrans: false, timestamp: 0, payload: 500},
			{now: 616_000, seq: 2, retrans: false, timestamp: 0, payload: 84},
			{now: 716_000, seq: 3, retrans: false, timestamp: 0, payload: 500},
		})

		stats := b.sender.Stats()
		require.Equal(t, uint64(4), stats.PktUnique)
		require.Equal(t, uint64(2*84+2*500), stats.ByteUnique)
		require.Equal(t, uint64(2*84+2*500), stats.ByteBuf)
		require.Equal(t, float64(516000), stats.UsPktSndPeriod)
	})
}
