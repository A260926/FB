/*
 * TypeAnnotationPath.java
 *
 * Copyright (c) 2024 Mike Strobel
 *
 * This source code is subject to terms and conditions of the Apache License, Version 2.0.
 * A copy of the license can be found in the License.html file at the root of this distribution.
 * By using this source code in any fashion, you are agreeing to be bound by the terms of the
 * Apache License, Version 2.0.
 *
 * You must not remove this notice, or any other, from this software.
 */

package com.strobel.assembler.metadata.typeannotations;

import com.strobel.core.VerifyArgument;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The decoded {@code type_path} structure of a type annotation (JVMS 4.7.20.2).
 *
 * <p>A path is an ordered sequence of {@link Entry steps} describing how to walk from
 * the root type named by the target to the annotated type usage.  An empty path means
 * the annotation applies to the root type itself.</p>
 */
public final class TypeAnnotationPath {

    /** The {@code type_path_kind} values defined in JVMS table 4.7.20-A. */
    public enum Kind {
        ARRAY_ELEMENT(0),
        INNER_TYPE(1),
        WILDCARD_BOUND(2),
        TYPE_ARGUMENT(3);

        private final int _code;

        Kind(final int code) {
            _code = code;
        }

        public int getCode() {
            return _code;
        }

        public static Kind fromCode(final int code) {
            for (final Kind kind : values()) {
                if (kind._code == code) {
                    return kind;
                }
            }

            throw new IllegalStateException(
                "Invalid type annotation type_path_kind: " + code
            );
        }
    }

    /** One {@code path_entry} ({@code type_path_kind}, {@code type_argument_index}) pair. */
    public static final class Entry {
        private final Kind _kind;
        private final int _typeArgumentIndex;

        public Entry(final Kind kind, final int typeArgumentIndex) {
            _kind = VerifyArgument.notNull(kind, "kind");
            _typeArgumentIndex = typeArgumentIndex;
        }

        public Kind getKind() {
            return _kind;
        }

        /**
         * The raw {@code type_argument_index} byte; only meaningful for
         * {@link Kind#TYPE_ARGUMENT}, zero for the other path kinds as mandated by JVMS.
         */
        public int getTypeArgumentIndex() {
            return _typeArgumentIndex;
        }

        @Override
        public String toString() {
            return _kind + (
                _kind == Kind.TYPE_ARGUMENT ? "(" + _typeArgumentIndex + ")" : ""
            );
        }
    }

    public static final TypeAnnotationPath EMPTY = new TypeAnnotationPath(
        Collections.<Entry>emptyList()
    );

    private final List<Entry> _entries;

    public TypeAnnotationPath(final List<Entry> entries) {
        _entries = Collections.unmodifiableList(
            new ArrayList<>(VerifyArgument.notNull(entries, "entries"))
        );
    }

    public List<Entry> getEntries() {
        return _entries;
    }

    public boolean isEmpty() {
        return _entries.isEmpty();
    }

    public int size() {
        return _entries.size();
    }

    public Entry get(final int index) {
        return _entries.get(index);
    }

    @Override
    public String toString() {
        if (_entries.isEmpty()) {
            return "[]";
        }

        final StringBuilder sb = new StringBuilder("[");

        for (int i = 0; i < _entries.size(); i++) {
            if (i > 0) {
                sb.append(", ");
            }
            sb.append(_entries.get(i));
        }

        return sb.append(']').toString();
    }
}
