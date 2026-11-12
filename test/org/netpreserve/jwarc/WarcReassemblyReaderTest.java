/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 National Library of Australia and the jwarc contributors
 */

package org.netpreserve.jwarc;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.ReadableByteChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.zip.GZIPOutputStream;

import static java.nio.charset.StandardCharsets.US_ASCII;
import static java.nio.charset.StandardCharsets.UTF_8;
import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

public class WarcReassemblyReaderTest {

    private static final String A = "00000000-0000-0000-0000-000000000001";
    private static final String A2 = "00000000-0000-0000-0000-000000000002";
    private static final String B = "00000000-0000-0000-0000-000000000003";
    private static final String B2 = "00000000-0000-0000-0000-000000000004";
    private static final String U = "00000000-0000-0000-0000-000000000005";
    private static final String ID6 = "00000000-0000-0000-0000-000000000006";
    private static final String ID7 = "00000000-0000-0000-0000-000000000007";
    private static final String URL = "http://example.org/capture";

    @Rule
    public TemporaryFolder temporaryFolder = new TemporaryFolder();

    // ------------------------------------------------------------- builders

    /** A physical WARC record assembled directly from bytes, exactly as it would appear in an archive. */
    private static byte[] rec(String type, String id, String extraHeaders, byte[] body) {
        StringBuilder header = new StringBuilder();
        header.append("WARC/1.1\r\n");
        header.append("WARC-Type: ").append(type).append("\r\n");
        header.append("WARC-Record-ID: <urn:uuid:").append(id).append(">\r\n");
        header.append("WARC-Date: 2025-01-01T00:00:00Z\r\n");
        header.append("Content-Length: ").append(body.length).append("\r\n");
        header.append(extraHeaders);
        header.append("\r\n");
        byte[] headerBytes = header.toString().getBytes(US_ASCII);
        byte[] trailer = "\r\n\r\n".getBytes(US_ASCII);
        byte[] out = Arrays.copyOf(headerBytes, headerBytes.length + body.length + trailer.length);
        System.arraycopy(body, 0, out, headerBytes.length, body.length);
        System.arraycopy(trailer, 0, out, headerBytes.length + body.length, trailer.length);
        return out;
    }

    private static byte[] rec(String type, String id, String extraHeaders, String body) {
        return rec(type, id, extraHeaders, body.getBytes(US_ASCII));
    }

    private static byte[] firstSegment(String type, String id, String target, byte[] body) {
        StringBuilder extra = new StringBuilder();
        if (target != null) {
            extra.append("WARC-Target-URI: ").append(target).append("\r\n");
        }
        extra.append("WARC-Segment-Number: 1\r\n");
        extra.append("Content-Type: application/http;msgtype=response\r\n");
        extra.append("WARC-Block-Digest: sha1:BLOCK-").append(id, 0, 1).append("\r\n");
        extra.append("WARC-Payload-Digest: sha1:PAYLOAD-").append(id, 0, 1).append("\r\n");
        return rec(type, id, extra.toString(), body);
    }

    private static byte[] continuation(String id, String originId, long segmentNumber, byte[] body,
                                       Long totalLength) {
        StringBuilder extra = new StringBuilder();
        extra.append("WARC-Target-URI: ").append(URL).append("\r\n");
        extra.append("WARC-Segment-Number: ").append(segmentNumber).append("\r\n");
        extra.append("WARC-Segment-Origin-ID: <urn:uuid:").append(originId).append(">\r\n");
        if (totalLength != null) {
            extra.append("WARC-Segment-Total-Length: ").append(totalLength).append("\r\n");
        }
        return rec("continuation", id, extra.toString(), body);
    }

    private static byte[] concat(byte[]... parts) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] part : parts) {
            out.write(part);
        }
        return out.toByteArray();
    }

    /** Gzips each physical record independently so the archive uses record-at-a-time gzip members. */
    private static byte[] gzipEachRecord(byte[] warc) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] record : splitRecords(warc)) {
            try (GZIPOutputStream gz = new GZIPOutputStream(out)) {
                gz.write(record);
            }
        }
        return out.toByteArray();
    }

    /** Gzips the whole archive as a single member (still a valid WARC.gz for the reader). */
    private static byte[] gzipWhole(byte[] warc) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (GZIPOutputStream gz = new GZIPOutputStream(out)) {
            gz.write(warc);
        }
        return out.toByteArray();
    }

    private static List<byte[]> splitRecords(byte[] warc) {
        List<byte[]> records = new ArrayList<>();
        int start = 0;
        while (start < warc.length) {
            int headerEnd = indexOf(warc, "\r\n\r\n".getBytes(US_ASCII), start) + 4;
            String header = new String(warc, start, headerEnd - start, US_ASCII);
            int contentLength = Integer.parseInt(findHeader(header, "Content-Length"));
            int end = headerEnd + contentLength + 4; // body plus the trailing CRLFCRLF
            records.add(Arrays.copyOfRange(warc, start, end));
            start = end;
        }
        return records;
    }

    private static int indexOf(byte[] haystack, byte[] needle, int from) {
        outer:
        for (int i = from; i <= haystack.length - needle.length; i++) {
            for (int j = 0; j < needle.length; j++) {
                if (haystack[i + j] != needle[j]) {
                    continue outer;
                }
            }
            return i;
        }
        throw new IllegalArgumentException("delimiter not found");
    }

    private static String findHeader(String headerBlock, String name) {
        for (String line : headerBlock.split("\r\n")) {
            if (line.toLowerCase().startsWith(name.toLowerCase() + ":")) {
                return line.substring(line.indexOf(':') + 1).trim();
            }
        }
        throw new IllegalArgumentException("missing header " + name);
    }

    private static byte[] readBody(WarcRecord record) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] tmp = new byte[13]; // deliberately awkward buffer size
        InputStream in = record.body().stream();
        int n;
        while ((n = in.read(tmp)) != -1) {
            out.write(tmp, 0, n);
        }
        return out.toByteArray();
    }

    /** Immutable snapshot taken while the reader is open, so assertions survive reader.close(). */
    private static final class Captured {
        final Class<?> cls;
        final String type;
        final URI id;
        final String target;
        final java.time.Instant date;
        final byte[] body;
        final Map<String, List<String>> headers;

        Captured(WarcRecord record) throws IOException {
            this.cls = record.getClass();
            this.type = record.type();
            this.id = record.id();
            this.target = record instanceof WarcTargetRecord ? ((WarcTargetRecord) record).target() : null;
            this.date = record.date();
            this.body = readBody(record);
            this.headers = record.headers().map();
        }

        String sole(String name) {
            return headers.getOrDefault(name, java.util.Collections.emptyList())
                    .stream().findFirst().orElse(null);
        }
    }

    private WarcReassemblyReader open(byte[] warc, String folder) throws IOException {
        Path tempDir = temporaryFolder.newFolder(folder).toPath();
        return new WarcReassemblyReader(new WarcReader(new ByteArrayInputStream(warc)), tempDir);
    }

    private static Path[] listFiles(Path dir) throws IOException {
        try (java.util.stream.Stream<Path> stream = Files.list(dir)) {
            return stream.toArray(Path[]::new);
        }
    }

    private List<Captured> captureAll(byte[] warc, boolean gzipped, String label) throws IOException {
        byte[] data = gzipped ? gzipEachRecord(warc) : warc;
        List<Captured> captured = new ArrayList<>();
        try (WarcReassemblyReader reader = open(data, label + "-" + (gzipped ? "gz" : "plain"))) {
            Optional<WarcRecord> next;
            while ((next = reader.next()).isPresent()) {
                captured.add(new Captured(next.get()));
            }
        }
        return captured;
    }

    /** Runs the same assertions against the plain archive and the per-record gzip archive. */
    private void forBothWays(byte[] warc, Asserter asserter) throws IOException {
        asserter.check(captureAll(warc, false, "case"));
        asserter.check(captureAll(warc, true, "case"));
    }

    private interface Asserter {
        void check(List<Captured> records) throws IOException;
    }

    private static void assertSameSnapshots(List<Captured> plain, List<Captured> gz) {
        assertEquals(plain.size(), gz.size());
        for (int i = 0; i < plain.size(); i++) {
            Captured p = plain.get(i);
            Captured g = gz.get(i);
            assertEquals(p.cls, g.cls);
            assertEquals(p.type, g.type);
            assertEquals(p.id, g.id);
            assertArrayEquals(p.body, g.body);
        }
    }

    // ----------------------------------------------------------------- tests

    /** A1, plain U, B1, B2(terminal), A2(terminal): head-of-queue ordering with interleaved chains and a plain record. */
    @Test
    public void interleavedChainsAndPlainRecordKeepFirstSeenOrder() throws IOException {
        byte[] bodyA1 = "AAAA1".getBytes(US_ASCII);
        byte[] bodyA2 = "AAAA2".getBytes(US_ASCII);
        byte[] bodyB1 = "BBBB1".getBytes(US_ASCII);
        byte[] bodyB2 = "BBBB2".getBytes(US_ASCII);
        byte[] bodyU = "PLAIN".getBytes(US_ASCII);

        byte[] warc = concat(
                firstSegment("response", A, URL, bodyA1),
                rec("resource", U, "WARC-Target-URI: http://example.org/plain\r\n", bodyU),
                firstSegment("resource", B, URL, bodyB1),
                continuation(B2, B, 2, bodyB2, (long) bodyB1.length + bodyB2.length),
                continuation(A2, A, 2, bodyA2, (long) bodyA1.length + bodyA2.length));

        Asserter asserter = records -> {
            assertEquals(3, records.size());

            Captured first = records.get(0);
            assertEquals(WarcResponse.class, first.cls);
            assertEquals("urn:uuid:" + A, first.id.toString());
            assertArrayEquals(concat(bodyA1, bodyA2), first.body);
            // segment control fields and the now invalid block digest are removed, payload digest is kept
            assertEquals(null, first.sole("WARC-Segment-Number"));
            assertEquals(null, first.sole("WARC-Segment-Origin-ID"));
            assertEquals(null, first.sole("WARC-Segment-Total-Length"));
            assertEquals(null, first.sole("WARC-Block-Digest"));
            assertEquals("sha1:PAYLOAD-0", first.sole("WARC-Payload-Digest"));
            assertEquals("10", first.sole("Content-Length"));
            assertEquals(URL, first.target);
            assertEquals(java.time.Instant.parse("2025-01-01T00:00:00Z"), first.date);

            Captured second = records.get(1);
            assertEquals(WarcResource.class, second.cls);
            assertEquals("urn:uuid:" + U, second.id.toString());
            assertArrayEquals(bodyU, second.body);
            // unsegmented records keep every header, including the original Content-Length
            assertEquals("5", second.sole("Content-Length"));

            Captured third = records.get(2);
            assertEquals(WarcResource.class, third.cls);
            assertEquals("urn:uuid:" + B, third.id.toString());
            assertArrayEquals(concat(bodyB1, bodyB2), third.body);
            assertEquals("10", third.sole("Content-Length"));
        };
        forBothWays(warc, asserter);
        assertSameSnapshots(captureAll(warc, false, "cmp"), captureAll(warc, true, "cmp"));
    }

    /** Two captures of the very same URL are two distinct chains, never merged. */
    @Test
    public void sameUrlCapturesAreNotMerged() throws IOException {
        byte[] c1a = "first-capture-part-1".getBytes(US_ASCII);
        byte[] c1b = "first-capture-part-2".getBytes(US_ASCII);
        byte[] c2a = "second-capture-part-1".getBytes(US_ASCII);
        byte[] c2b = "second-capture-part-2".getBytes(US_ASCII);

        byte[] warc = concat(
                firstSegment("response", A, URL, c1a),
                firstSegment("response", B, URL, c2a),
                continuation(A2, A, 2, c1b, (long) c1a.length + c1b.length),
                continuation(B2, B, 2, c2b, (long) c2a.length + c2b.length));

        forBothWays(warc, records -> {
            assertEquals(2, records.size());
            assertEquals("urn:uuid:" + A, records.get(0).id.toString());
            assertArrayEquals(concat(c1a, c1b), records.get(0).body);
            assertEquals("urn:uuid:" + B, records.get(1).id.toString());
            assertArrayEquals(concat(c2a, c2b), records.get(1).body);
        });
    }

    /** The split falls inside an HTTP header line and inside a chunked transfer-encoding length line. */
    @Test
    public void splitAcrossHttpHeaderAndChunkedBoundaries() throws IOException {
        String fullResponse =
                "HTTP/1.1 200 OK\r\n"
                + "Content-Type: text/plain\r\n"
                + "Transfer-Encoding: chunked\r\n"
                + "\r\n"
                + "4\r\nWiki\r\n"
                + "5\r\npedia\r\n"
                + "0\r\n\r\n";
        byte[] full = fullResponse.getBytes(US_ASCII);
        // cut 1: inside the "Content-Type" header line; cut 2: between the "5" and the CRLF of a chunk length line
        int cut1 = 22;
        int cut2 = indexOf(full, "5\r\n".getBytes(US_ASCII), 0) + 1;
        assertTrue(cut2 > cut1);
        byte[] part1 = Arrays.copyOfRange(full, 0, cut1);
        byte[] part2 = Arrays.copyOfRange(full, cut1, cut2);
        byte[] part3 = Arrays.copyOfRange(full, cut2, full.length);

        byte[] warc = concat(
                firstSegment("response", A, URL, part1),
                continuation(ID6, A, 2, part2, null),
                continuation(ID7, A, 3, part3, (long) full.length));

        for (boolean gzipped : new boolean[]{false, true}) {
            byte[] data = gzipped ? gzipEachRecord(warc) : warc;
            try (WarcReassemblyReader reader = open(data, "chunked-" + gzipped)) {
                WarcResponse response = (WarcResponse) reader.next().get();

                // http() must parse a response whose header was split across WARC segments
                HttpResponse http = response.http();
                assertEquals(200, http.status());
                assertEquals("text/plain", http.contentType().toString());

                // payload() must decode the chunked body whose length line was split
                ByteArrayOutputStream payload = new ByteArrayOutputStream();
                InputStream in = response.payload().get().body().stream();
                byte[] tmp = new byte[7];
                int n;
                while ((n = in.read(tmp)) != -1) {
                    payload.write(tmp, 0, n);
                }
                assertEquals("Wikipedia", payload.toString("US-ASCII"));
                assertFalse(reader.next().isPresent());
            }
            // a fresh pass checks the exact reassembled raw bytes
            List<Captured> captured = captureAll(warc, gzipped, "chunked-bytes-" + gzipped);
            assertEquals(1, captured.size());
            assertArrayEquals(full, captured.get(0).body);
            assertEquals(WarcResponse.class, captured.get(0).cls);
        }
    }

    /** Ordinary records, including zero-length bodies, pass through with untouched headers and exact bytes. */
    @Test
    public void unsegmentedRecordsAreDeliveredUnchanged() throws IOException {
        byte[] warc = concat(
                rec("warcinfo", U, "Content-Type: application/warc-fields\r\n", "software: test\r\n"),
                rec("metadata", A2, "WARC-Target-URI: " + URL + "\r\n", new byte[0]),
                rec("response", B, "WARC-Target-URI: " + URL + "\r\nContent-Type: application/http\r\n",
                        "HTTP/1.0 204 No Content\r\n\r\n"));

        forBothWays(warc, records -> {
            assertEquals(3, records.size());
            assertEquals(Warcinfo.class, records.get(0).cls);
            assertArrayEquals("software: test\r\n".getBytes(US_ASCII), records.get(0).body);
            assertEquals(WarcMetadata.class, records.get(1).cls);
            assertEquals(0, records.get(1).body.length);
            assertEquals(WarcResponse.class, records.get(2).cls);
            assertArrayEquals("HTTP/1.0 204 No Content\r\n\r\n".getBytes(US_ASCII), records.get(2).body);
        });

        // the response still parses as HTTP (checked on a live record, before the reader is closed)
        for (boolean gzipped : new boolean[]{false, true}) {
            byte[] data = gzipped ? gzipEachRecord(warc) : warc;
            try (WarcReassemblyReader reader = open(data, "plain-http-" + gzipped)) {
                assertEquals(Warcinfo.class, reader.next().get().getClass());
                assertEquals(WarcMetadata.class, reader.next().get().getClass());
                WarcResponse response = (WarcResponse) reader.next().get();
                assertEquals(204, response.http().status());
            }
        }
    }

    /** Reading part of a body then calling next() closes the previous body; an early caller close is tolerated. */
    @Test
    public void partialReadAndEarlyCloseDoNotBreakDelivery() throws IOException {
        byte[] firstBody = new byte[100];
        Arrays.fill(firstBody, (byte) 'x');
        byte[] secondBody = new byte[50];
        Arrays.fill(secondBody, (byte) 'y');
        byte[] warc = concat(
                rec("resource", A, "WARC-Target-URI: " + URL + "\r\n", firstBody),
                rec("resource", B, "WARC-Target-URI: " + URL + "\r\n", secondBody));

        try (WarcReassemblyReader reader = open(warc, "partial")) {
            WarcRecord first = reader.next().get();
            byte[] peek = new byte[10];
            assertEquals(10, first.body().read(ByteBuffer.wrap(peek)));
            first.body().close(); // caller closes early, as permitted

            WarcRecord second = reader.next().get();
            assertArrayEquals(secondBody, readBody(second));
            assertFalse(reader.next().isPresent());
        }
    }

    /** close() removes only the files it created and always closes the source reader. */
    @Test
    public void closeCleansTempFilesButKeepsExistingOnes() throws IOException {
        Path tempDir = temporaryFolder.newFolder("cleanup").toPath();
        Path preExisting = tempDir.resolve("user-file.txt");
        Files.write(preExisting, "keep me".getBytes(UTF_8));

        byte[] part1 = "one".getBytes(US_ASCII);
        byte[] part2 = "two".getBytes(US_ASCII);
        byte[] warc = concat(
                firstSegment("response", A, URL, part1),
                continuation(A2, A, 2, part2, (long) part1.length + part2.length));

        WarcReader source = new WarcReader(new ByteArrayInputStream(warc));
        WarcReassemblyReader reader = new WarcReassemblyReader(source, tempDir);
        WarcRecord record = reader.next().get();
        assertArrayEquals("onetwo".getBytes(US_ASCII), readBody(record));

        Path[] created = listFiles(tempDir);
        Path[] onlyCreated = Arrays.stream(created)
                .filter(p -> !p.equals(preExisting))
                .toArray(Path[]::new);
        assertTrue("a temporary file must have been created", onlyCreated.length >= 1);
        for (Path p : onlyCreated) {
            assertTrue(Files.exists(p));
        }

        reader.close();
        for (Path p : onlyCreated) {
            assertFalse("temporary file must be deleted: " + p, Files.exists(p));
        }
        assertTrue("pre-existing directory content must survive", Files.exists(preExisting));
        assertFalse("source reader must be closed", source.channel().isOpen());

        reader.close(); // safe to call twice
    }

    /** A chain missing its terminal segment at EOF throws and nothing behind it is delivered. */
    @Test
    public void unfinishedChainAtEofThrows() throws IOException {
        byte[] warc = concat(
                firstSegment("response", A, URL, "head".getBytes(US_ASCII)),
                rec("resource", U, "WARC-Target-URI: " + URL + "\r\n", "trapped"));

        for (boolean gzipped : new boolean[]{false, true}) {
            byte[] data = gzipped ? gzipEachRecord(warc) : warc;
            try (WarcReassemblyReader reader = open(data, "eof-" + gzipped)) {
                try {
                    reader.next();
                    fail("expected IOException for an unfinished chain (gzipped=" + gzipped + ")");
                } catch (IOException e) {
                    assertTrue("message should identify the chain, got: " + e.getMessage(),
                            e.getMessage().contains("urn:uuid:" + A));
                }
            }
        }
    }

    @Test
    public void orphanContinuationIsRejected() throws IOException {
        byte[] warc = continuation(A2, A, 2, "x".getBytes(US_ASCII), 1L);
        assertBadChain(warc, A);
    }

    @Test
    public void continuationWithoutOriginIdIsRejected() throws IOException {
        byte[] warc = rec("continuation", A2, "WARC-Segment-Number: 2\r\n", "x".getBytes(US_ASCII));
        assertBadChain(warc, A2);
    }

    @Test
    public void selfReferencingOriginIdIsRejected() throws IOException {
        byte[] warc = concat(
                firstSegment("response", A, URL, "x".getBytes(US_ASCII)),
                rec("continuation", A2, "WARC-Segment-Number: 2\r\n"
                        + "WARC-Segment-Origin-ID: <urn:uuid:" + A2 + ">\r\n", "y".getBytes(US_ASCII)));
        assertBadChain(warc, A2);
    }

    @Test
    public void duplicateChainStartIsRejected() throws IOException {
        byte[] block = "x".getBytes(US_ASCII);
        byte[] warc = concat(
                firstSegment("response", A, URL, block),
                firstSegment("response", A, URL, block));
        assertBadChain(warc, A);
    }

    @Test
    public void gapInSegmentNumbersIsRejected() throws IOException {
        byte[] warc = concat(
                firstSegment("response", A, URL, "a".getBytes(US_ASCII)),
                continuation(A2, A, 3, "b".getBytes(US_ASCII), 2L));
        assertBadChain(warc, A);
    }

    @Test
    public void duplicateSegmentNumberIsRejected() throws IOException {
        byte[] warc = concat(
                firstSegment("response", A, URL, "a".getBytes(US_ASCII)),
                continuation(A2, A, 2, "b".getBytes(US_ASCII), null),
                continuation(ID6, A, 2, "c".getBytes(US_ASCII), 3L));
        assertBadChain(warc, A);
    }

    @Test
    public void wrongTotalLengthIsRejected() throws IOException {
        byte[] warc = concat(
                firstSegment("response", A, URL, "abc".getBytes(US_ASCII)),
                continuation(A2, A, 2, "de".getBytes(US_ASCII), 99L));
        assertBadChain(warc, A);
    }

    @Test
    public void segmentedUnsupportedTypeIsRejected() throws IOException {
        byte[] warc = rec("request", A, "WARC-Segment-Number: 1\r\n"
                + "WARC-Target-URI: " + URL + "\r\n", "GET / HTTP/1.0\r\n\r\n".getBytes(US_ASCII));
        assertBadChain(warc, A);
    }

    @Test
    public void declaredContentLengthMismatchIsRejected() throws IOException {
        // Content-Length claims 4 bytes but the block ends after 3: the physical record's body cannot be satisfied.
        byte[] truncated = ("WARC/1.1\r\n"
                + "WARC-Type: response\r\n"
                + "WARC-Record-ID: <urn:uuid:" + A + ">\r\n"
                + "WARC-Date: 2025-01-01T00:00:00Z\r\n"
                + "Content-Length: 4\r\n"
                + "WARC-Segment-Number: 1\r\n"
                + "WARC-Target-URI: " + URL + "\r\n"
                + "\r\nabc").getBytes(US_ASCII);
        for (boolean gzipped : new boolean[]{false, true}) {
            byte[] data = gzipped ? gzipWhole(truncated) : truncated;
            try (WarcReassemblyReader reader = open(data, "short-block-" + gzipped)) {
                try {
                    reader.next();
                    fail("expected IOException for a short content block (gzipped=" + gzipped + ")");
                } catch (IOException e) {
                    assertTrue("error must locate the chain or segment, got: " + e.getMessage(),
                            e.getMessage().contains("urn:uuid:" + A));
                }
            }
        }
    }

    private void assertBadChain(byte[] warc, String chainIdFragment) throws IOException {
        for (boolean gzipped : new boolean[]{false, true}) {
            byte[] data = gzipped ? gzipEachRecord(warc) : warc;
            try (WarcReassemblyReader reader = open(data, "bad-" + gzipped)) {
                try {
                    reader.next();
                    fail("expected IOException (gzipped=" + gzipped + ")");
                } catch (IOException e) {
                    assertTrue("error must locate the chain or segment id, got: " + e.getMessage(),
                            e.getMessage().contains(chainIdFragment));
                }
                // failed state is sticky
                try {
                    reader.next();
                    fail("next() after a failure must keep throwing");
                } catch (IOException expected) {
                    // expected
                }
            }
        }
    }

    /**
     * A capture much larger than the fixed working buffer is spooled through a counting channel. The assertions prove
     * the source is only ever read in fixed-buffer-sized requests and the complete body lands in a temporary file
     * rather than in memory.
     */
    @Test
    public void largeBodyNeverResidesWhollyInMemory() throws IOException {
        int totalSize = 200_000; // far larger than the 8192 byte working buffers
        int cut = 77_777;
        byte[] warc = buildLargeSegmentedWarc(totalSize, cut);

        CountingChannel counter = new CountingChannel(Channels.newChannel(new ByteArrayInputStream(warc)));
        Path tempDir = temporaryFolder.newFolder("large").toPath();
        try (WarcReassemblyReader reader = new WarcReassemblyReader(new WarcReader(counter), tempDir)) {
            WarcRecord record = reader.next().get();
            assertTrue(record instanceof WarcResponse);
            assertEquals(totalSize, record.body().size());

            // the reassembled block was spooled to disk in full
            Path[] spooled = listFiles(tempDir);
            assertEquals(1, spooled.length);
            assertEquals(totalSize, Files.size(spooled[0]));

            // verify the exact contents in fixed-size pieces
            byte[] expected = new byte[2048];
            byte[] actual = new byte[2048];
            InputStream body = record.body().stream();
            long checked = 0;
            while (checked < totalSize) {
                int want = (int) Math.min(expected.length, totalSize - checked);
                for (int i = 0; i < want; i++) {
                    expected[i] = (byte) (checked + i);
                }
                int got = body.read(actual, 0, want);
                assertTrue(got > 0);
                for (int i = 0; i < got; i++) {
                    assertEquals(expected[i], actual[i]);
                }
                checked += got;
            }
            assertEquals(totalSize, checked);
            assertEquals(-1, body.read());
            assertFalse(reader.next().isPresent());
        }

        assertTrue("the source was never asked for more than the fixed buffer in one read (max="
                + counter.maxRead + ")", counter.maxRead <= 8192);
        assertTrue("the 200k body was streamed, never held in a single buffer (max read "
                + counter.maxRead + ")", counter.maxRead < totalSize / 10);
    }

    private byte[] buildLargeSegmentedWarc(int totalSize, int cut) {
        byte[] body = new byte[totalSize];
        for (int i = 0; i < totalSize; i++) {
            body[i] = (byte) i;
        }
        byte[] part1 = Arrays.copyOfRange(body, 0, cut);
        byte[] part2 = Arrays.copyOfRange(body, cut, totalSize);
        return concat(
                firstSegment("response", A, URL, part1),
                continuation(A2, A, 2, part2, (long) totalSize));
    }

    /** ReadableByteChannel that records the largest single read request made against the underlying stream. */
    private static final class CountingChannel implements ReadableByteChannel {
        private final ReadableByteChannel delegate;
        private int maxRead;

        CountingChannel(ReadableByteChannel delegate) {
            this.delegate = delegate;
        }

        @Override
        public int read(ByteBuffer dst) throws IOException {
            int n = delegate.read(dst);
            if (n > maxRead) {
                maxRead = n;
            }
            return n;
        }

        @Override
        public boolean isOpen() {
            return delegate.isOpen();
        }

        @Override
        public void close() throws IOException {
            delegate.close();
        }
    }

    @Test
    public void emptyArchiveYieldsEmptyOptional() throws IOException {
        try (WarcReassemblyReader reader = open(new byte[0], "empty")) {
            assertFalse(reader.next().isPresent());
        }
    }

    @Test
    public void threeSegmentChainReassemblesInOrder() throws IOException {
        byte[] p1 = "alpha-".getBytes(US_ASCII);
        byte[] p2 = "beta-".getBytes(US_ASCII);
        byte[] p3 = "gamma".getBytes(US_ASCII);
        byte[] warc = concat(
                firstSegment("resource", A, URL, p1),
                continuation(ID6, A, 2, p2, null),
                continuation(ID7, A, 3, p3, (long) p1.length + p2.length + p3.length));

        forBothWays(warc, records -> {
            assertEquals(1, records.size());
            Captured record = records.get(0);
            assertEquals(WarcResource.class, record.cls);
            assertArrayEquals("alpha-beta-gamma".getBytes(US_ASCII), record.body);
            assertEquals("16", record.sole("Content-Length"));
        });
    }

    @Test
    public void gzipMemberBoundariesDoNotLeakIntoBody() throws IOException {
        byte[] p1 = "part-one-".getBytes(US_ASCII);
        byte[] p2 = "part-two".getBytes(US_ASCII);
        byte[] warc = concat(
                firstSegment("response", A, URL, p1),
                continuation(A2, A, 2, p2, (long) p1.length + p2.length));
        byte[] gzipped = gzipEachRecord(warc);

        // sanity: the compressed archive really contains two independent gzip members
        GunzipChannel gunzip = new GunzipChannel(
                Channels.newChannel(new ByteArrayInputStream(gzipped)), ByteBuffer.allocate(8192).flip());
        ByteBuffer drain = ByteBuffer.allocate(1024);
        while (gunzip.read(drain) >= 0) {
            drain.clear();
        }
        assertEquals(2, gunzip.memberCount());

        try (WarcReassemblyReader reader = open(gzipped, "boundaries")) {
            WarcResponse response = (WarcResponse) reader.next().get();
            assertArrayEquals("part-one-part-two".getBytes(US_ASCII), readBody(response));
            assertFalse(reader.next().isPresent());
        }
    }

    @Test
    public void constructorRejectsNullArguments() throws IOException {
        Path tempDir = temporaryFolder.getRoot().toPath();
        try {
            new WarcReassemblyReader(null, tempDir);
            fail("null source should be rejected");
        } catch (IllegalArgumentException expected) {
            // expected
        }
        try {
            new WarcReassemblyReader(
                    new WarcReader(Channels.newChannel(new ByteArrayInputStream(new byte[0]))), null);
            fail("null tempDirectory should be rejected");
        } catch (IllegalArgumentException expected) {
            // expected
        }
    }
}
