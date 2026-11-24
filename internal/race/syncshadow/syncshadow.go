package syncshadow

import (
	"sync"
)

// SyncShadow manages shadow memory for synchronization primitives.
//
// This maps each sync primitive address (uintptr) to its SyncVar, which
// tracks the release clock for happens-before tracking.
//
// Implementation:
//   - Uses sync.Map for lock-free concurrent access
//   - SyncVar allocated on first access to a mutex address
//   - Never freed (mutexes typically live for program lifetime)
//
// Memory Model:
//   - Key: uintptr (address of sync.Mutex, sync.RWMutex, etc.)
//   - Value: *SyncVar (tracks release clock)
//
// Thread Safety: All methods are safe for concurrent calls.
//
// Example:
//
//	shadow := NewSyncShadow()
//	mutexAddr := uintptr(unsafe.Pointer(&mu))
//	sv := shadow.GetOrCreate(mutexAddr)
//	sv.SetReleaseClock(ctx.C)  // On Unlock
//	ctx.C.Join(sv.GetReleaseClock())  // On Lock
type SyncShadow struct {
	// vars maps sync primitive addresses to their SyncVar instances.
	// Key: uintptr (address of sync primitive)
	// Value: *SyncVar (release clock tracking)
	//
	// Using sync.Map for lock-free concurrent access patterns:
	//   - Most accesses are reads (GetOrCreate on repeated Lock/Unlock)
	//   - Writes are rare (only on first access to a new mutex)
	//
	// sync.Map is optimized for this "stable keys" pattern.
	vars sync.Map
}

// NewSyncShadow creates and initializes a new SyncShadow instance.
//
// The shadow memory is initially empty. SyncVar entries are created lazily
// on first access to each mutex address.
//
// Example:
//
//	shadow := NewSyncShadow()
//	sv := shadow.GetOrCreate(0x1234)  // Creates new SyncVar
//	sv2 := shadow.GetOrCreate(0x1234) // Returns same SyncVar
func NewSyncShadow() *SyncShadow {
	return &SyncShadow{}
}

// GetOrCreate returns the SyncVar for the given address, creating it if needed.
//
// This is the primary entry point for accessing sync variable state.
// On first access to an address, a new SyncVar is allocated and stored.
// Subsequent accesses return the existing SyncVar.
//
// Parameters:
//   - addr: Address of the synchronization primitive (e.g., &mutex)
//
// Returns:
//   - *SyncVar: The sync variable for this address (never nil)
//
// Thread Safety: Safe for concurrent calls. Multiple goroutines may race
// to create the SyncVar, but sync.Map.LoadOrStore ensures only one is used.
//
// Performance:
//   - First access: ~100ns (allocation + sync.Map.LoadOrStore)
//   - Cached access: ~10ns (sync.Map.Load)
//   - Zero allocations on cached access
//
// Example:
//
//	shadow := NewSyncShadow()
//	sv1 := shadow.GetOrCreate(0x1234)  // Allocates SyncVar
//	sv2 := shadow.GetOrCreate(0x1234)  // Returns same SyncVar
//	assert(sv1 == sv2)
func (s *SyncShadow) GetOrCreate(addr uintptr) *SyncVar {
	// Try to load existing SyncVar (fast path).
	if val, ok := s.vars.Load(addr); ok {
		return val.(*SyncVar)
	}

	// Slow path: Create new SyncVar for this address.
	// Multiple goroutines may race here, but LoadOrStore ensures
	// only one SyncVar is actually used.
	newVar := &SyncVar{}
	val, _ := s.vars.LoadOrStore(addr, newVar)
	return val.(*SyncVar)
}

// Reset clears all sync variable state.
//
// This removes all SyncVar entries from shadow memory. Used primarily
// for testing to ensure clean state between test cases.
//
// Thread Safety: NOT safe for concurrent access. The caller must ensure
// no other goroutines are using the shadow memory.
//
// Example:
//
//	shadow := NewSyncShadow()
//	shadow.GetOrCreate(0x1234)
//	shadow.Reset()
//	// All SyncVar entries are now cleared
func (s *SyncShadow) Reset() {
	// Create a new sync.Map to clear all entries.
	// This is more efficient than Range + Delete for large maps.
	s.vars = sync.Map{}
}
