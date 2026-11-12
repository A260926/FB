/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 National Library of Australia and the jwarc contributors
 */

package org.netpreserve.jwarc;

import java.io.Closeable;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeMap;

import static java.nio.file.StandardOpenOption.CREATE;
import static java.nio.file.StandardOpenOption.READ;
import static java.nio.file.StandardOpenOption.WRITE;

/**
 * A reader that reassembles segmented WARC records into single logical records.
 * <p>
 * A single HTTP response (or resource) may be stored in a WARC file as a first {@code response} or {@code resource}
 * record ({@code WARC-Segment-Number: 1}) followed by one or more {@code continuation} records pointing back at it with
 * {@code WARC-Segment-Origin-ID}. Other captures may be interleaved between the segments. This reader wraps an ordinary
 * {@link WarcReader} and transparently joins the segments of a capture back together, so callers only ever see complete
 * records whose body can be read independently.
 * <p>
 * Supported segmented types are {@code response} and {@code resource} in WARC 1.0 and 1.1. The first segment must have
 * a segment number of 1 and its Record-ID is the identity of the whole chain. Continuation records must reference that
 * id with {@code WARC-Segment-Origin-ID} and their segment numbers must increase one at a time. A chain is completed by
 * a continuation carrying {@code WARC-Segment-Total-Length} whose value must equal the exact sum of the byte lengths of
 * all content blocks in the chain. Chains are never merged by target URL: two captures of the same URL remain two
 * chains, and a segment's own Record-ID may not be used as its Origin-ID. Out-of-order delivery, chains that end
 * without a terminal segment and segmented records of other types are not supported and result in an {@link IOException}.
 * <p>
 * Records are returned in the order in which the first segment of each chain (or each ordinary, unsegmented record) was
 * first encountered; a chain that finishes early never overtakes an earlier chain that is still open. The body of the
 * record returned by the previous call to {@link #next()} is consumed and closed before the next record is returned,
 * mirroring {@link WarcReader}.
 * <p>
 * Reassembled bodies are spooled to temporary files inside the supplied directory through a fixed-size working buffer,
 * so neither a single capture nor the set of chains waiting in parallel ever has to fit into memory. Temporary files are
 * removed by {@link #close()}; files that already existed in the directory are left untouched.
 * <p>
 * Once an error has been reported this reader enters a failed state: every subsequent call to {@link #next()} throws
 * without attempting to read further captures.
 * <p>
 * This class is not safe for use by multiple threads.
 */
public class WarcReassemblyReader implements Closeable {
    private static final int BUFFER_SIZE = 8192;
    private static final Map<String, WarcRecord.Constructor> CONSTRUCTORS = initConstructors();

    private final WarcReader source;
    private final Path tempDirectory;
    private final ByteBuffer copyBuffer = ByteBuffer.allocate(BUFFER_SIZE);

    /** Chains and ordinary records keyed by Record-ID, in first-seen order. Only the head may be delivered. */
    private final LinkedHashMap<String, Chain> queue = new LinkedHashMap<>();
    /** Every physical Record-ID seen so far, in angle-bracket form. */
    private final HashSet<String> seenIds = new HashSet<>();
    /** Temporary files created by this reader, in creation order. */
    private final List<Path> tempFiles = new ArrayList<>();
    /** Every body view handed out, so close() can release them all. */
    private final List<TempFileMessageBody> allBodies = new ArrayList<>();

    /** The last logical record handed out; its body is closed on the next call to next(). */
    private WarcRecord delivered;
    private boolean failed;
    private boolean closed;
    private boolean sourceExhausted;

    /**
     * Creates a reassembling reader.
     *
     * @param source        the reader supplying the physical WARC records
     * @param tempDirectory directory in which temporary files holding reassembled bodies are created; must exist
     */
    public WarcReassemblyReader(WarcReader source, Path tempDirectory) {
        if (source == null) throw new IllegalArgumentException("source must not be null");
        if (tempDirectory == null) throw new IllegalArgumentException("tempDirectory must not be null");
        this.source = source;
        this.tempDirectory = tempDirectory;
    }

    /**
     * Reads the next complete logical record.
     * <p>
     * The body of any record returned by a previous call is closed first. The returned record is always complete: if
     * its final segment has not been reached yet, more physical records are read until it is.
     *
     * @return the next reassembled or ordinary record, or an empty Optional at the end of the underlying WARC stream
     * @throws IOException if a segment is malformed, a chain is left unfinished at end of file, or an I/O error occurs
     */
    public Optional<WarcRecord> next() throws IOException {
        if (failed) {
            throw new IOException("WarcReassemblyReader is in a failed state following a previous error");
        }
        if (closed) {
            throw new IOException("WarcReassemblyReader is closed");
        }
        try {
            return nextRecord();
        } catch (IOException e) {
            failed = true;
            throw e;
        } catch (RuntimeException e) {
            failed = true;
            throw new IOException("invalid WARC record encountered during reassembly: " + e.getMessage(), e);
        }
    }

    private Optional<WarcRecord> nextRecord() throws IOException {
        closeDelivered();

        while (true) {
            // Only the oldest entry may be delivered: a chain that completes early must not overtake an earlier
            // chain that is still open. Entries behind a completed head are drained on subsequent calls.
            if (!queue.isEmpty()) {
                Chain head = queue.values().iterator().next();
                if (head.complete) {
                    queue.remove(head.originId);
                    delivered = buildRecord(head);
                    return Optional.of(delivered);
                }
            }

            if (sourceExhausted) {
                if (!queue.isEmpty()) {
                    Chain chain = queue.values().iterator().next();
                    throw fail("unfinished segmented record at end of file: chain " + chain.originId
                            + " received " + chain.receivedSegments + " segment(s) but has no terminal "
                            + "WARC-Segment-Total-Length");
                }
                return Optional.empty();
            }

            Optional<WarcRecord> maybeRecord = source.next();
            if (!maybeRecord.isPresent()) {
                sourceExhausted = true;
                continue;
            }
            ingest(maybeRecord.get());
        }
    }

    private void ingest(WarcRecord record) throws IOException {
        String recordId = record.headers().sole("WARC-Record-ID").orElse(null);
        if (recordId == null || recordId.isEmpty()) {
            throw fail("record without a WARC-Record-ID (WARC-Type: " + safeType(record) + ")");
        }
        if (!seenIds.add(recordId)) {
            throw fail("duplicate WARC-Record-ID " + recordId);
        }

        Optional<Long> segmentNumber = record.segmentNumber();
        if (!segmentNumber.isPresent()) {
            // An ordinary, unsegmented record. Its body has to be spooled immediately as the underlying WarcReader
            // closes it when the next physical record is read. It is delivered with its headers untouched, but chains
            // that started earlier take priority.
            Chain plain = new Chain(recordId);
            plain.first = record;
            plain.version = record.version();
            plain.receivedSegments = 1;
            plain.bodyLength = spool(record, plain, 1);
            plain.complete = true;
            plain.unsegmented = true;
            queue.put(recordId, plain);
            return;
        }

        long number = segmentNumber.get();
        String type = record.type();
        if (number == 1) {
            if (!"response".equals(type) && !"resource".equals(type)) {
                throw fail("segmented records of type " + type + " are not supported (first segment " + recordId + ")");
            }
            Chain chain = new Chain(recordId);
            chain.first = record;
            chain.version = record.version();
            chain.receivedSegments = 1;
            chain.expectedNextNumber = 2;
            chain.bodyLength = spool(record, chain, 1);
            queue.put(recordId, chain);
            return;
        }

        // Any later segment must be a continuation referencing a chain that is still open.
        if (!(record instanceof WarcContinuation)) {
            throw fail("segment number " + number + " on a non-continuation record " + recordId
                    + " is not supported; only the first segment may use another WARC-Type");
        }
        String originId = record.headers().sole("WARC-Segment-Origin-ID").orElse(null);
        if (originId == null || originId.isEmpty()) {
            throw fail("continuation " + recordId + " segment " + number + " is missing a WARC-Segment-Origin-ID");
        }
        if (originId.equals(recordId)) {
            throw fail("continuation " + recordId + " segment " + number
                    + " uses its own WARC-Record-ID as the WARC-Segment-Origin-ID");
        }
        Chain chain = queue.get(originId);
        if (chain == null || chain.complete) {
            throw fail("orphan continuation " + recordId + " segment " + number
                    + " references unknown or already finished chain " + originId);
        }
        if (number != chain.expectedNextNumber) {
            if (number <= chain.receivedSegments) {
                throw fail("duplicate segment " + number + " of chain " + originId + " (record " + recordId + ")");
            }
            throw fail("segment numbers in chain " + originId + " must increase by one: expected segment "
                    + chain.expectedNextNumber + " but got segment " + number + " (record " + recordId + ")");
        }

        spool(record, chain, number);
        chain.receivedSegments++;
        chain.expectedNextNumber++;

        Optional<Long> totalLength = ((WarcContinuation) record).segmentTotalLength();
        if (totalLength.isPresent()) {
            if (totalLength.get() != chain.bodyLength) {
                throw fail("WARC-Segment-Total-Length mismatch in chain " + originId + ": header declares "
                        + totalLength.get() + " byte(s) but the content blocks of the " + chain.receivedSegments
                        + " segments total " + chain.bodyLength + " byte(s)");
            }
            chain.complete = true;
        }
    }

    /**
     * Copies the full content block of a physical record into the chain's temporary file, verifying that the block
     * actually contains exactly the number of bytes declared by its Content-Length header. Returns the number of bytes
     * copied.
     */
    private long spool(WarcRecord record, Chain chain, long segmentNumber) throws IOException {
        MessageBody body = record.body();
        long declared = body.size();
        if (declared < 0) {
            declared = record.headers().sole("Content-Length").map(Long::parseLong).orElse(-1L);
        }

        if (chain.output == null) {
            Path path = Files.createTempFile(tempDirectory, "jwarc-reassembly-", ".tmp");
            tempFiles.add(path);
            chain.path = path;
            chain.output = FileChannel.open(path, CREATE, READ, WRITE);
        }
        FileChannel out = chain.output;

        long written = 0;
        copyBuffer.clear();
        int n;
        try {
            while ((n = body.read(copyBuffer)) >= 0) {
                if (n == 0) {
                    continue; // a channel may legally return zero reads; try again
                }
                copyBuffer.flip();
                while (copyBuffer.hasRemaining()) {
                    out.write(copyBuffer);
                }
                written += n;
                copyBuffer.clear();
            }
        } catch (IOException e) {
            throw new IOException("failed reading the content block of segment " + segmentNumber
                    + " of chain " + chain.originId + ": " + e.getMessage(), e);
        }

        // The physical record must end exactly at its declared Content-Length. A short or long block means the WARC
        // framing or gzip member boundary was corrupted; reject it with a locatable chain and segment number.
        if (declared >= 0 && written != declared) {
            throw fail("segment " + segmentNumber + " of chain " + chain.originId + " contains " + written
                    + " body byte(s) but its Content-Length header declares " + declared);
        }
        return written;
    }

    private WarcRecord buildRecord(Chain chain) throws IOException {
        try {
            chain.output.flush();

            WarcRecord first = chain.first;
            Map<String, List<String>> headerMap = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
            for (Map.Entry<String, List<String>> entry : first.headers().map().entrySet()) {
                String name = entry.getKey();
                if (!chain.unsegmented && (isSegmentControlHeader(name)
                        || "WARC-Block-Digest".equalsIgnoreCase(name)
                        || "Content-Length".equalsIgnoreCase(name))) {
                    continue;
                }
                headerMap.put(name, new ArrayList<>(entry.getValue()));
            }
            if (!chain.unsegmented) {
                headerMap.put("Content-Length", Collections.singletonList(Long.toString(chain.bodyLength)));
            }
            MessageHeaders headers = new MessageHeaders(headerMap);

            TempFileMessageBody body = TempFileMessageBody.create(chain.path);
            allBodies.add(body);

            WarcRecord.Constructor constructor = CONSTRUCTORS.get(first.type());
            if (constructor == null) {
                constructor = CONSTRUCTORS.get("default");
            }
            WarcRecord result = constructor.construct(chain.version, headers, body);
            result.recordSource = first.recordSource;
            return result;
        } finally {
            // Spooling is finished; release the write channel even if constructing the record fails. The read views
            // handed to callers open their own channels and are tracked separately.
            if (chain.output != null) {
                chain.output.close();
                chain.output = null;
            }
        }
    }

    private static boolean isSegmentControlHeader(String name) {
        return "WARC-Segment-Number".equalsIgnoreCase(name)
                || "WARC-Segment-Origin-ID".equalsIgnoreCase(name)
                || "WARC-Segment-Total-Length".equalsIgnoreCase(name);
    }

    private void closeDelivered() throws IOException {
        if (delivered != null) {
            MessageBody body = delivered.body();
            try {
                if (body.isOpen()) {
                    body.consume();
                }
            } finally {
                body.close();
                delivered = null;
            }
        }
    }

    private IOException fail(String message) {
        failed = true;
        return new IOException(message);
    }

    private static String safeType(WarcRecord record) {
        try {
            return record.type();
        } catch (RuntimeException e) {
            return "<unknown>";
        }
    }

    private static Map<String, WarcRecord.Constructor> initConstructors() {
        Map<String, WarcRecord.Constructor> map = new HashMap<>();
        map.put("default", WarcRecord::new);
        map.put("continuation", WarcContinuation::new);
        map.put("conversion", WarcConversion::new);
        map.put("metadata", WarcMetadata::new);
        map.put("request", WarcRequest::new);
        map.put("resource", WarcResource::new);
        map.put("response", WarcResponse::new);
        map.put("revisit", WarcRevisit::new);
        map.put("warcinfo", Warcinfo::new);
        return map;
    }

    /**
     * Closes the underlying reader and deletes every temporary file created by this reader.
     * <p>
     * Files in the temporary directory that this reader did not create are never deleted. Safe to call more than once.
     */
    @Override
    public void close() throws IOException {
        if (closed) {
            return;
        }
        closed = true;
        IOException suppressed = null;

        if (delivered != null) {
            try {
                delivered.body().close();
            } catch (IOException e) {
                suppressed = e;
            }
            delivered = null;
        }

        for (TempFileMessageBody body : allBodies) {
            try {
                body.close();
            } catch (IOException e) {
                if (suppressed == null) suppressed = e;
            }
        }

        for (Chain chain : queue.values()) {
            if (chain.output != null) {
                try {
                    chain.output.close();
                } catch (IOException e) {
                    if (suppressed == null) suppressed = e;
                }
            }
        }

        for (Path path : tempFiles) {
            try {
                Files.deleteIfExists(path);
            } catch (IOException e) {
                if (suppressed == null) suppressed = e;
            }
        }

        try {
            source.close();
        } catch (IOException e) {
            if (suppressed == null) suppressed = e;
        }

        if (suppressed != null) {
            throw suppressed;
        }
    }

    /**
     * Mutable bookkeeping for one chain of segmented records, or a queue entry for an ordinary unsegmented record.
     */
    private static final class Chain {
        final String originId;
        WarcRecord first;
        MessageVersion version;
        Path path;
        FileChannel output;
        long bodyLength;
        long receivedSegments;
        long expectedNextNumber;
        boolean complete;
        /** True when this entry is an ordinary unsegmented record whose headers must be preserved verbatim. */
        boolean unsegmented;

        Chain(String originId) {
            this.originId = originId;
        }
    }
}
