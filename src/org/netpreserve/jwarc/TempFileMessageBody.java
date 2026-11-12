/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 National Library of Australia and the jwarc contributors
 */

package org.netpreserve.jwarc;

import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.FileChannel;
import java.nio.file.Path;

import static java.nio.file.StandardOpenOption.READ;

/**
 * A fixed-length message body backed by a temporary file.
 * <p>
 * Each instance opens its own file channel with an independent read position, so several bodies (for example
 * reassembled records waiting to be delivered) can coexist and be closed without affecting each other. Closing a body
 * closes only this view's channel; it never deletes the file. The owning reader is responsible for removing temporary
 * files.
 */
class TempFileMessageBody extends MessageBody {
    private final FileChannel file;
    private final Path path;
    private final long size;
    private long position;
    private boolean open = true;

    TempFileMessageBody(FileChannel file, Path path, long size) {
        this.file = file;
        this.path = path;
        this.size = size;
    }

    static TempFileMessageBody create(Path path) throws IOException {
        FileChannel file = FileChannel.open(path, READ);
        boolean success = false;
        try {
            TempFileMessageBody body = new TempFileMessageBody(file, path, file.size());
            success = true;
            return body;
        } finally {
            if (!success) {
                file.close();
            }
        }
    }

    Path path() {
        return path;
    }

    @Override
    public synchronized int read(ByteBuffer dst) throws IOException {
        if (!open) {
            throw new ClosedChannelException();
        }
        if (position >= size) {
            return -1;
        }
        int savedLimit = dst.limit();
        try {
            dst.limit(dst.position() + (int) Math.min(dst.remaining(), size - position));
            int n = file.read(dst, position);
            if (n < 0) {
                throw new EOFException("temporary body file ended after " + position + " of " + size + " bytes");
            }
            position += n;
            return n;
        } finally {
            dst.limit(savedLimit);
        }
    }

    @Override
    public boolean isOpen() {
        return open && file.isOpen();
    }

    @Override
    public void close() throws IOException {
        if (open) {
            open = false;
            file.close();
        }
    }

    @Override
    public long size() {
        return size;
    }

    @Override
    public long position() {
        return position;
    }
}
