- Date: 2020-02-02
- Authors: Artem Krylysov

# About

This document is a new version of the initial Pogreb design
[blog post](https://artem.krylysov.com/blog/2018/03/24/pogreb-key-value-store/) published in 2018.

The new version replaces the unstructured data file for storing key-value pairs with a write-ahead log to achieve
durability.

# Overview

Pogreb is an embedded key-value store for read-heavy workloads.
It aims to provide fast point lookups by indexing keys in an on-disk hash table.

# Design

Two key components of Pogreb are a write-ahead log (WAL) and a hash table index.
The WAL stores key-value pairs on disk in append-only files.
The on-disk hash table allows constant time lookups from keys to key-value pairs in the WAL.

## Write-ahead log

The WAL consists of multiple append-only segments. Once the current segment file is full (reaches 4 GB), a new segment
is created, the full segment becomes read-only.

```
Write-ahead log
+-----------+-----------+-...-+-----------+
| Segment 0 | Segment 1 | ... | Segment N |
+-----------+-----------+-...-+-----------+
```

### Segment

A segment is a sequence of variable-length binary-encoded records.

```
Segment
+----------+----------+-...-+----------+
| Record 0 | Record 1 | ... | Record N |
+----------+----------+-...-+----------+
```

The record layout:

```
Record
+---------------+------------------+------------------+-...-+--...--+----------+
| Key Size (2B) | Record Type (1b) | Value Size (31b) | Key | Value | CRC (4B) |
+---------------+------------------+------------------+-...-+--...--+----------+
```

The Record Type field is either `Put` (0) or `Delete` (1).

## Hash table index

Pogreb uses two files to store the hash table on disk - "main" and "overflow" index files.

Each index file holds an array of buckets.

```
Index
+----------+----------+-...-+----------+
| Bucket 0 | Bucket 1 | ... | Bucket N |
+----------+----------+-...-+----------+
```

### Bucket

A bucket is an array of slots followed by an optional file pointer to the overflow bucket (stored in the "overflow"
index).
The number of slots in a bucket is 31 - that is the maximum number of slots that is possible to fit in 512
bytes.

```
Bucket
+--------+--------+-...-+--------+-----------------------------+
| Slot 0 | Slot 1 | ... | Slot N | Overflow Bucket Offset (8B) |
+--------+--------+-...-+--------+-----------------------------+
```

### Slot

A slot contains the hash, the size of the key, the value size and a 32-bit offset of the key-value pair in the WAL.

```
Slot
+-----------+-----------------+---------------+-----------------+-------------+
| Hash (4B) | Segment ID (2B) | Key Size (2B) | Value Size (4B) | Offset (4B) |
+-----------+-----------------+---------------+-----------------+-------------+
```

## Linear hashing

Pogreb uses the [Linear hashing](https://en.wikipedia.org/wiki/Linear_hashing) algorithm which grows the hash table
one bucket at a time instead of rebuilding it entirely.

Initially, the hash table contains a single bucket (*N=1*).

Level *L* (initially *L=0*) represents the maximum number of buckets on a logarithmic scale the hash table can store.
For example, a hash table with *L=0* contains between 0 and 1 buckets; *L=3* contains between 4 and 8 buckets.

*S* is the index of the "split" bucket (initially *S=0*).

Collisions are resolved using the bucket chaining technique.
The "overflow" index file stores overflow buckets that form a linked list.

### Lookup

Position of a bucket in the index file is calculated by applying a hash function to a key:

```
          Index
          +----------+
          | Bucket 0 |    Bucket
          +----------+    +--------+--------+-...-+--------+
h(key) -> | Bucket 1 | -> | Slot 0 | Slot 1 | ... | Slot N |
          +-........-+    +--------+--------+-...-+--------+
          | ........ |                    |
          +-........-+                    |
          | Bucket N |                    |
          +----------+                    |
                                          v
                                Write-ahead log
                                +-----------+-----------+-...-+-----------+
                                | Segment 0 | Segment 1 | ... | Segment N |
                                +-----------+-----------+-...-+-----------+
```

To get the position of the bucket:

1. Hash the key (Pogreb uses the 32-bit version of MurmurHash3).
2. Use 2<sup>L</sup> bits of the hash to get the position of the bucket - `hash % math.Pow(2, L)`.
3. Set the position to `hash % math.Pow(2, L+1)` if the previously calculated position comes before the
split bucket *S*.

The lookup function reads a bucket at the given position from the index file and performs a linear search to find a slot
with the required hash.
If the bucket doesn't contain a slot with the required hash, but the pointer to the overflow bucket is non-zero, the
overflow bucket is inspected.
The process continues until a required slot is found or until there is no more overflow buckets for the given key.
Once a slot with the required key is found, Pogreb reads the key-value pair from the WAL.

The average lookup requires two I/O operations - one is to find a slot in the index and another one is to read the key
and value from the WAL.

### Insertion

Insertion is performed by adding a new "put" record to the WAL and updating a bucket in the index.
If the bucket has all of its slots occupied, a new overflow bucket is created.

### Split

When the number of items in the hash table exceeds the load factor threshold (70%), the split operation is performed on
the split bucket *S*:

1. Allocate a new bucket at the end of the index file.
2. Increment the split bucket index *S*.
3. Increment *L* and reset *S* to 0 if *S* points to 2<sup>L</sup>. 
4. Divide items from the old split bucket between the newly allocated bucket and the old split bucket by
recalculating the positions of the keys in the hash table.
5. Increment the number of buckets *N*.

### Removal

The removal operation lookups a bucket by key, removes a slot from the bucket, overwrites the bucket in the index
and then appends a new "delete" record to the WAL.

## Compaction

Since the WAL is append-only, the disk space occupied by overwritten or deleted keys is not reclaimed immediately.
Pogreb supports optional online compaction.

Every time a key is overwritten or deleted, Pogreb increments the number of "deleted" bytes and keys for the
corresponding WAL segment.
The background compaction thread periodically loops through the WAL segment metadata and picks segments with 50% or
higher disk space fragmentation for compaction.
The compaction thread finds segment's live records (not deleted or overwritten) by looking up keys in the index.
It writes live records to a new segment file and updates the corresponding slots in the index file.
After the compaction is successfully finished, the compacted segment files are removed.

## Backup

A backup is a consistent copy of the database as it existed at one point in time, written to a separate directory as
recoverable data segments followed by a lock file marker. The destination contains no index; opening it replays the
segments and rebuilds the index, so a backup never ships an index that could have changed after the backup boundary.

### Capture boundary

`Backup` starts with a short critical section. Under the database and datalog locks it builds an immutable manifest
listing every segment that exists at that moment. Each manifest entry records the segment identity - the physical
segment id and the monotonic sequence id - together with the segment name and its exact byte length, and acquires one
retention reference on the segment. No file data is read or copied while a lock is held.

After the manifest is captured, all I/O runs lock-free: segment files are opened, streamed through a fixed-size
buffer with `ReadAt`, and closed one at a time. A slow destination therefore does not block `Put`, `Delete` or
`Compact`, and a backup never holds more than one extra source handle and one destination handle at a time.

The captured byte length is what makes the snapshot consistent. A segment that is still active (not full) keeps
growing as new records are appended, but the backup copies only the bytes present at the boundary. Records appended
afterwards, and keys inserted, overwritten or deleted afterwards, are not part of the backup.

### Segment retention and reclamation

Compaction retires segments: it detaches them from the current dataset, closes them and deletes their files, and the
freed physical segment id is quickly reused by a new segment. A backup that captured a segment must still be able to
read the old generation, while compaction must not be prevented from evicting that segment from the live dataset.

Every segment carries a backup reference count and a `retired` flag, both guarded by the datalog mutex.

- Capturing a manifest increments the reference count of every segment on it, including active segments.
- Compaction marks a segment `retired` and removes it from the live segment table immediately. If references remain,
  the file and its metadata stay on disk and the handle stays open; otherwise the segment is purged right away.
- When a backup finishes (successfully or with an error) it releases its references. The last release of a retired
  segment closes it and removes its data and metadata files. Retired segments never linger in the normal compaction
  list.

Two backups with different destinations can overlap and capture different boundaries. Each holds its own references,
so one backup finishing or failing never releases a file another backup still needs; only the last reference triggers
the cleanup.

The sequence id keeps generations apart. A retained segment and a new segment reusing the same physical id coexist on
disk under different file names (`id-sequenceID.psg`); the manifest opens the captured name, so a backup can never
read the new generation through a recycled id.

Retirement does not rely on platform "unlink while open" behavior, on opening every source file up front, or on
copying records into memory or a shadow directory: retention is an explicit reference count, only one source file is
open per backup at a time, and the only in-memory state is the manifest plus the fixed copy buffer.

## Recovery

In the event of a crash caused by a power loss or an operating system failure, Pogreb discards the index and replays the
WAL building a new index from scratch.
Segments are iterated from the oldest to the newest and items are inserted into the index.

# Limitations

The design choices made to optimize for point lookups bring limitations for other potential use-cases. For example, using a hash table for indexing makes range scans impossible. Additionally, having a single hash table shared across all WAL segments makes the recovery process require rebuilding the entire index, which may be impractical for large databases.