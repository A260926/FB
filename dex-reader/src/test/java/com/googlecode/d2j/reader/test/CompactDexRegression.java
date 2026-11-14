package com.googlecode.d2j.reader.test;

import com.googlecode.d2j.DexLabel;
import com.googlecode.d2j.Method;
import com.googlecode.d2j.reader.BaseDexFileReader;
import com.googlecode.d2j.reader.DexFileReader;
import com.googlecode.d2j.reader.MultiDexFileReader;
import com.googlecode.d2j.reader.Op;
import com.googlecode.d2j.visitors.DexClassVisitor;
import com.googlecode.d2j.visitors.DexCodeVisitor;
import com.googlecode.d2j.visitors.DexDebugVisitor;
import com.googlecode.d2j.visitors.DexFileVisitor;
import com.googlecode.d2j.visitors.DexMethodVisitor;

import task_support.Check;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Regression coverage for offline CompactDex v001 reading.
 *
 * <p>
 * Every image used here is generated in this class from the public container format: the images
 * are inert byte data, they are never loaded or executed. The same logical content is emitted
 * both as an ordinary dex and as a CompactDex image so that the existing visitor event streams
 * can be compared directly.
 * </p>
 */
public final class CompactDexRegression {

    private CompactDexRegression() {
    }

    // ---------------------------------------------------------------
    // Visitor event collector
    // ---------------------------------------------------------------

    static final class Events {
        final List<String> events = new ArrayList<>();
        final Map<String, List<Integer>> linesByMethod = new LinkedHashMap<>();
        final Map<String, Integer> debugVisitsByMethod = new LinkedHashMap<>();

        void accept(BaseDexFileReader reader, int config) {
            reader.accept(new DexFileVisitor() {
                @Override
                public DexClassVisitor visit(int access, String name, String parent, String[] interfaces) {
                    events.add("CLASS " + name + " EXTENDS " + parent);
                    return new DexClassVisitor() {
                        @Override
                        public void visitSource(String file) {
                            events.add("SOURCE " + file);
                        }

                        @Override
                        public DexMethodVisitor visitMethod(int flags, Method method) {
                            final String mname = method.getName();
                            events.add("METHOD " + method);
                            linesByMethod.computeIfAbsent(mname, k -> new ArrayList<Integer>());
                            debugVisitsByMethod.merge(mname, 0, Integer::sum);
                            return new DexMethodVisitor() {
                                @Override
                                public DexCodeVisitor visitCode() {
                                    return new DexCodeVisitor() {
                                        @Override
                                        public void visitRegister(int total) {
                                            events.add("REGISTERS " + total);
                                        }

                                        @Override
                                        public void visitConstStmt(Op op, int register, Object value) {
                                            events.add("CONST " + op.displayName + " " + register + " " + value);
                                        }

                                        @Override
                                        public void visitStmt0R(Op op) {
                                            events.add("STMT0R " + op.displayName);
                                        }

                                        @Override
                                        public void visitStmt1R(Op op, int reg) {
                                            events.add("STMT1R " + op.displayName + " " + reg);
                                        }

                                        @Override
                                        public void visitLabel(DexLabel label) {
                                            events.add("LABEL " + label);
                                        }

                                        @Override
                                        public void visitTryCatch(DexLabel start, DexLabel end, DexLabel[] handlers,
                                                String[] types) {
                                            events.add("TRY " + start + " " + end + " " + Arrays.toString(handlers)
                                                    + " " + Arrays.toString(types));
                                        }

                                        @Override
                                        public DexDebugVisitor visitDebug() {
                                            debugVisitsByMethod.merge(mname, 1, Integer::sum);
                                            return new DexDebugVisitor() {
                                                @Override
                                public void visitLineNumber(int line, DexLabel label) {
                                                    events.add("LINE " + line);
                                                    linesByMethod.get(mname).add(line);
                                                }
                                            };
                                        }
                                    };
                                }
                            };
                        }
                    };
                }
            }, config);
        }

        void accept(BaseDexFileReader reader, int classIdx, int config) {
            reader.accept(new DexFileVisitor() {
                @Override
                public DexClassVisitor visit(int access, String name, String parent, String[] interfaces) {
                    events.add("CLASS " + name + " EXTENDS " + parent);
                    return new DexClassVisitor() {
                        @Override
                        public DexMethodVisitor visitMethod(int flags, Method method) {
                            final String mname = method.getName();
                            linesByMethod.computeIfAbsent(mname, k -> new ArrayList<Integer>());
                            return new DexMethodVisitor() {
                                @Override
                                public DexCodeVisitor visitCode() {
                                    return new DexCodeVisitor() {
                                        @Override
                                        public DexDebugVisitor visitDebug() {
                                            return new DexDebugVisitor() {
                                                @Override
                                                public void visitLineNumber(int line, DexLabel label) {
                                                    events.add("LINE " + line);
                                                    linesByMethod.get(mname).add(line);
                                                }
                                            };
                                        }
                                    };
                                }
                            };
                        }
                    };
                }
            }, classIdx, config);
        }
    }

    static Events collect(byte[] image) {
        Events e = new Events();
        e.accept(new DexFileReader(image), 0);
        return e;
    }

    // ---------------------------------------------------------------
    // Self-contained legal image material
    // ---------------------------------------------------------------

    static final class MethodSpec {
        final String name;
        final String[] params;
        final String ret;
        final short[] insns;
        final byte[] debug; // null means no debug info
        final int localRegs;
        final int insWords;
        final int regDelta;
        final int outsExt;
        final int triesExt;
        final boolean insnsExt;
        final int tryStart;
        final int tryEnd;
        final int handlerAddr;
        final String handlerType; // null means no try

        MethodSpec(String name, String[] params, String ret, short[] insns, byte[] debug, int localRegs,
                int insWords, int regDelta, int outsExt, int triesExt, boolean insnsExt, int tryStart, int tryEnd,
                int handlerAddr, String handlerType) {
            this.name = name;
            this.params = params;
            this.ret = ret;
            this.insns = insns;
            this.debug = debug;
            this.localRegs = localRegs;
            this.insWords = insWords;
            this.regDelta = regDelta;
            this.outsExt = outsExt;
            this.triesExt = triesExt;
            this.insnsExt = insnsExt;
            this.tryStart = tryStart;
            this.tryEnd = tryEnd;
            this.handlerAddr = handlerAddr;
            this.handlerType = handlerType;
        }

        boolean hasTry() {
            return handlerType != null;
        }
    }

    /** Standard DEX debug info: given start line, no parameter names and one line at address zero. */
    static byte[] debugOneLine(int line) {
        // line, zero parameters, special opcode 0x0e (same address, same line), end sequence.
        return new byte[] { (byte) line, 0, 0x0e, 0x00 };
    }

    /** Debug info with one parameter, line 10 at address 0 and line 12 at address 1. */
    static byte[] debugTryMethod() {
        // line 10, one unnamed parameter, 0x0e -> line 10 @0, 0x1f -> addr +1 / line +2 -> 12 @1.
        return new byte[] { 10, 1, 0, 0x0e, 0x1f, 0x00 };
    }

    static List<MethodSpec> buildSpecs() {
        List<MethodSpec> methods = new ArrayList<>();
        for (int i = 0; i < 17; i++) {
            byte[] debug = null;
            if (i == 0) {
                debug = debugOneLine(42);
            } else if (i == 15) {
                debug = debugOneLine(43);
            } else if (i == 16) {
                debug = debugOneLine(44);
            }
            // return-void, one local register, no debug info for most methods.
            methods.add(new MethodSpec("m" + i, new String[0], "V", new short[] { 0x0e00 }, debug, 1, 0, 0, 0,
                    0, false, 0, 0, 0, null));
        }
        // Compact header whose preheader carries a register delta (+1), an outs extension
        // (+1) and the two-word instruction count extension (high part zero).
        methods.add(new MethodSpec("extFields", new String[0], "V", new short[] { 0x1270, 0x0f00 }, null, 1, 0,
                1, 1, 0, true, 0, 0, 0, null));
        // Method with an exception range; the tries count comes purely from the one-word tries
        // extension, and debug info is resolved through the sparse table.
        methods.add(new MethodSpec("tryMethod", new String[] { "I" }, "I", new short[] { 0x1270, 0x0f00 },
                debugTryMethod(), 1, 1, 0, 0, 1, false, 0, 1, 1, "Ljava/lang/RuntimeException;"));
        return methods;
    }

    static final class DataOut {
        final ByteBuffer bb;

        DataOut(int cap) {
            this.bb = ByteBuffer.allocate(cap).order(ByteOrder.LITTLE_ENDIAN);
        }

        int pos() {
            return bb.position();
        }

        void align(int n) {
            while (pos() % n != 0) {
                bb.put((byte) 0);
            }
        }

        void uleb(int x) {
            do {
                int q = x & 127;
                x >>>= 7;
                bb.put((byte) (q | (x == 0 ? 0 : 128)));
            } while (x != 0);
        }
    }

    static final class ProtoDef {
        final List<Integer> paramTypeIds = new ArrayList<>();
        final int returnTypeId;
        int typeListOff;

        ProtoDef(int returnTypeId) {
            this.returnTypeId = returnTypeId;
        }
    }

    /**
     * Emit the same logical content as an ordinary dex ({@code compact == false}) or CompactDex
     * v001 ({@code compact == true}). CompactDex payload offsets stay relative to the data
     * region; ordinary dex offsets are file absolute.
     */
    static byte[] buildImage(boolean compact, List<MethodSpec> specs) {
        final String source = "A.java";
        final String classType = "LA;";
        final String superType = "Ljava/lang/Object;";
        final String runtime = "Ljava/lang/RuntimeException;";

        Map<String, Integer> strings = new LinkedHashMap<>();
        for (String s : new String[] { source, "I", "V", classType, superType, runtime }) {
            strings.put(s, strings.size());
        }
        for (MethodSpec m : specs) {
            putString(strings, m.name);
            putString(strings, m.ret);
            for (String p : m.params) {
                putString(strings, p);
            }
        }
        Map<String, Integer> types = new LinkedHashMap<>();
        for (String t : new String[] { "I", "V", classType, superType, runtime }) {
            types.put(t, types.size());
        }
        List<ProtoDef> protoDefs = new ArrayList<>();
        Map<String, Integer> protoIndex = new LinkedHashMap<>();
        for (MethodSpec m : specs) {
            String key = Arrays.toString(m.params) + "->" + m.ret;
            if (!protoIndex.containsKey(key)) {
                ProtoDef def = new ProtoDef(types.get(m.ret));
                for (String p : m.params) {
                    def.paramTypeIds.add(types.get(p));
                }
                protoIndex.put(key, protoDefs.size());
                protoDefs.add(def);
            }
        }

        // Main-region layout is known before the data section, so the data start used to make
        // ordinary (non-compact) payload offsets file-absolute can be computed up front.
        int headerSizeEarly = compact ? 0x88 : 0x70;
        int stringIdsOffE = headerSizeEarly;
        int typeIdsOffE = stringIdsOffE + strings.size() * 4;
        int protoIdsOffE = typeIdsOffE + types.size() * 4;
        int methodIdsOffE = protoIdsOffE + protoDefs.size() * 12;
        int classDefsOffE = methodIdsOffE + specs.size() * 8;
        final int dataStartEarly = (classDefsOffE + 32 + 3) & ~3;

        // ---- data section (offsets here are relative to the data region) ----
        DataOut data = new DataOut(16384);

        int firstStringData = data.pos();
        int[] stringDataOff = new int[strings.size()];
        int si = 0;
        for (String s : strings.keySet()) {
            stringDataOff[si++] = data.pos();
            data.uleb(s.length());
            data.bb.put(s.getBytes(StandardCharsets.US_ASCII));
            data.bb.put((byte) 0);
        }

        for (ProtoDef def : protoDefs) {
            if (!def.paramTypeIds.isEmpty()) {
                data.align(4);
                def.typeListOff = data.pos();
                data.bb.putInt(def.paramTypeIds.size());
                for (int tid : def.paramTypeIds) {
                    data.bb.putShort((short) (int) tid);
                }
                data.align(4);
            }
        }

        int[] debugOff = new int[specs.size()];
        int debugCount = 0;
        int firstDebug = 0;
        for (int i = 0; i < specs.size(); i++) {
            if (specs.get(i).debug != null) {
                if (debugCount == 0) {
                    firstDebug = data.pos();
                }
                debugOff[i] = data.pos();
                data.bb.put(specs.get(i).debug);
                debugCount++;
            }
        }

        // CompactDex sparse debug offset table: one presence block per 16 method ids.
        int sparsePos = 0;
        int lookupOff = 0;
        if (compact) {
            data.align(4);
            sparsePos = data.pos();
            int blocks = (specs.size() + 15) / 16;
            int[] blockStart = new int[blocks];
            for (int b = 0; b < blocks; b++) {
                blockStart[b] = data.pos() - sparsePos;
                int presence = 0;
                for (int slot = 0; slot < 16; slot++) {
                    int idx = b * 16 + slot;
                    if (idx < specs.size() && specs.get(idx).debug != null) {
                        presence |= 1 << slot;
                    }
                }
                data.bb.put((byte) (presence >> 8)); // big-endian presence mask
                data.bb.put((byte) presence);
                int running = 0; // debug_info_base is zero for these images
                for (int slot = 0; slot < 16; slot++) {
                    int idx = b * 16 + slot;
                    if (idx < specs.size() && specs.get(idx).debug != null) {
                        int delta = debugOff[idx] - running;
                        data.uleb(delta);
                        running += delta;
                    }
                }
            }
            data.align(4);
            lookupOff = data.pos() - sparsePos;
            for (int b = 0; b < blocks; b++) {
                data.bb.putInt(blockStart[b]);
            }
            data.align(4);
        }

        int[] codeOff = new int[specs.size()];
        int firstCode = 0;
        for (int i = 0; i < specs.size(); i++) {
            MethodSpec m = specs.get(i);
            data.align(4);
            if (compact) {
                int flags = 0;
                if (m.regDelta != 0) {
                    flags |= 1 << 3;
                }
                if (m.insnsExt) {
                    flags |= 1 << 2;
                }
                if (m.outsExt != 0) {
                    flags |= 1 << 1;
                }
                if (m.triesExt != 0) {
                    flags |= 1 << 0;
                }
                // Extension words immediately before the fixed two-word header, in forward
                // field order: tries, outs, instruction count (low then high uint16), register
                // delta adjacent to the header.
                if (m.triesExt != 0) {
                    data.bb.putShort((short) m.triesExt);
                }
                if (m.outsExt != 0) {
                    data.bb.putShort((short) m.outsExt);
                }
                if (m.insnsExt) {
                    data.bb.putShort((short) 0); // low word of the high instruction count
                    data.bb.putShort((short) 0); // high word
                }
                if (m.regDelta != 0) {
                    data.bb.putShort((short) m.regDelta);
                }
                codeOff[i] = data.pos();
                int triesNibble = m.hasTry() && m.triesExt == 0 ? 1 : 0;
                // The outs value is delivered through the extension word, so the base nibble is
                // zero for these items.
                int first = ((m.localRegs & 0xF) << 12) | ((m.insWords & 0xF) << 8) | (0 << 4)
                        | (triesNibble & 0xF);
                int second = ((m.insns.length & 0x7FF) << 5) | flags;
                data.bb.putShort((short) first);
                data.bb.putShort((short) second);
                for (short insn : m.insns) {
                    data.bb.putShort(insn);
                }
                if (m.hasTry()) {
                    writeTryPayload(data, m, types);
                }
            } else {
                codeOff[i] = data.pos();
                int registers = m.localRegs + m.regDelta + m.insWords;
                int outs = m.outsExt;
                int tries = m.hasTry() ? 1 : 0;
                data.bb.putShort((short) registers);
                data.bb.putShort((short) m.insWords);
                data.bb.putShort((short) outs);
                data.bb.putShort((short) tries);
                data.bb.putInt(debugOff[i] + (compact ? 0 : dataStartEarly));
                data.bb.putInt(m.insns.length);
                for (short insn : m.insns) {
                    data.bb.putShort(insn);
                }
                if (m.hasTry()) {
                    writeTryPayload(data, m, types);
                }
            }
            if (i == 0) {
                firstCode = codeOff[i];
            }
        }

        data.align(4);
        int classDataOff = data.pos();
        data.uleb(0); // static fields
        data.uleb(0); // instance fields
        data.uleb(specs.size()); // direct methods (all static)
        data.uleb(0); // virtual methods
        for (int i = 0; i < specs.size(); i++) {
            data.uleb(i == 0 ? 0 : 1); // method id diff
            data.uleb(0x9); // public static
            data.uleb(codeOff[i] + (compact ? 0 : dataStartEarly));
        }

        // Map list is the final aligned payload item.
        data.align(4);
        int mapOff = data.pos();
        List<int[]> rows = new ArrayList<>();
        rows.add(new int[] { 0x0000, 1, 0 });
        rows.add(new int[] { 0x0001, strings.size(), -1 });
        rows.add(new int[] { 0x0002, types.size(), -2 });
        rows.add(new int[] { 0x0003, protoDefs.size(), -3 });
        rows.add(new int[] { 0x0005, specs.size(), -4 });
        rows.add(new int[] { 0x0006, 1, -5 });
        int typeListCount = 0;
        int firstTypeList = 0;
        for (ProtoDef def : protoDefs) {
            if (def.typeListOff != 0) {
                if (typeListCount == 0) {
                    firstTypeList = def.typeListOff;
                }
                typeListCount++;
            }
        }
        if (typeListCount > 0) {
            rows.add(new int[] { 0x1001, typeListCount, firstTypeList });
        }
        rows.add(new int[] { 0x2002, strings.size(), firstStringData });
        if (debugCount > 0) {
            rows.add(new int[] { 0x2003, debugCount, firstDebug });
        }
        rows.add(new int[] { 0x2001, specs.size(), firstCode });
        rows.add(new int[] { 0x2000, 1, classDataOff });
        rows.add(new int[] { 0x1000, 1, mapOff });
        data.bb.putInt(rows.size());
        for (int[] r : rows) {
            data.bb.putShort((short) r[0]);
            data.bb.putShort((short) 0);
            data.bb.putInt(r[1]);
            data.bb.putInt(r[2] < 0 ? 0 : r[2]); // main-region tables fill real offsets below
        }
        int dataLen = data.pos();

        // ---- main region ----
        int headerSize = headerSizeEarly;
        int stringIdsOff = stringIdsOffE;
        int typeIdsOff = typeIdsOffE;
        int protoIdsOff = protoIdsOffE;
        int methodIdsOff = methodIdsOffE;
        int classDefsOff = classDefsOffE;
        int dataStart = dataStartEarly;
        int dataBase = compact ? Math.max(0x200, (dataStart + 0xFF) & ~0xFF) : 0;
        int end = (compact ? dataBase : dataStart) + dataLen;

        ByteBuffer out = ByteBuffer.allocate(end).order(ByteOrder.LITTLE_ENDIAN);
        out.put((compact ? "cdex001\0" : "dex\n035\0").getBytes(StandardCharsets.ISO_8859_1));
        out.putInt(8, 0); // checksum/signature are not consulted by the reader
        out.putInt(32, compact ? dataBase : end); // file_size
        out.putInt(36, headerSize);
        out.putInt(40, 0x12345678); // endian tag
        out.putInt(44, 0); // link size
        out.putInt(48, 0); // link off
        out.putInt(52, compact ? mapOff : dataStart + mapOff);
        out.putInt(56, strings.size());
        out.putInt(60, stringIdsOff);
        out.putInt(64, types.size());
        out.putInt(68, typeIdsOff);
        out.putInt(72, protoDefs.size());
        out.putInt(76, protoIdsOff);
        out.putInt(80, 0); // no field ids
        out.putInt(84, 0);
        out.putInt(88, specs.size());
        out.putInt(92, methodIdsOff);
        out.putInt(96, 1);
        out.putInt(100, classDefsOff);
        out.putInt(104, dataLen);
        out.putInt(108, compact ? dataBase : dataStart);

        for (int off : stringDataOff) {
            out.putInt(compact ? off : dataStart + off);
        }
        for (String t : types.keySet()) {
            out.putInt(strings.get(t));
        }
        int p = 0;
        for (ProtoDef def : protoDefs) {
            out.position(protoIdsOff + p * 12);
            out.putInt(strings.get(def.returnTypeId == types.get("V") ? "V" : "I")); // shorty (unused by reader)
            out.putInt(def.returnTypeId);
            out.putInt(compact || def.typeListOff == 0 ? def.typeListOff : dataStart + def.typeListOff);
            p++;
        }
        out.position(methodIdsOff);
        for (MethodSpec m : specs) {
            String key = Arrays.toString(m.params) + "->" + m.ret;
            out.putShort((short) (int) types.get(classType));
            out.putShort((short) (int) protoIndex.get(key));
            out.putInt(strings.get(m.name));
        }
        out.position(classDefsOff);
        out.putInt(types.get(classType));
        out.putInt(0x1); // public
        out.putInt(types.get(superType));
        out.putInt(0); // interfaces
        out.putInt(strings.get(source));
        out.putInt(0); // annotations
        out.putInt(compact ? classDataOff : dataStart + classDataOff);
        out.putInt(0); // static values

        if (compact) {
            out.putInt(0x70, 0); // feature flags
            out.putInt(0x74, sparsePos);
            out.putInt(0x78, lookupOff);
            out.putInt(0x7C, 0); // debug_info_base
            out.putInt(0x80, 0); // owned_data_begin
            out.putInt(0x84, dataLen); // owned_data_end
        }

        // Patch main-region table offsets into the map list copy.
        for (int i = 0; i < rows.size(); i++) {
            int[] r = rows.get(i);
            if (r[2] == -1) {
                r[2] = stringIdsOff;
            } else if (r[2] == -2) {
                r[2] = typeIdsOff;
            } else if (r[2] == -3) {
                r[2] = protoIdsOff;
            } else if (r[2] == -4) {
                r[2] = methodIdsOff;
            } else if (r[2] == -5) {
                r[2] = classDefsOff;
            } else if (!compact) {
                r[2] = dataStart + r[2];
            }
        }

        out.position(compact ? dataBase : dataStart);
        // data.bb is exactly dataLen bytes.
        out.put(data.bb.array(), 0, dataLen);

        // The map list lives inside the data copy; patch its rows after the copy so the patched
        // offsets are not overwritten.
        ByteBuffer mapPatch = out.duplicate().order(ByteOrder.LITTLE_ENDIAN);
        mapPatch.position((compact ? dataBase : dataStart) + mapOff + 4);
        for (int[] r : rows) {
            mapPatch.putShort((short) r[0]);
            mapPatch.putShort((short) 0);
            mapPatch.putInt(r[1]);
            mapPatch.putInt(r[2]);
        }

        return Arrays.copyOf(out.array(), end);
    }

    private static void writeTryPayload(DataOut data, MethodSpec m, Map<String, Integer> types) {
        if ((m.insns.length & 1) != 0) {
            data.bb.putShort((short) 0); // padding
        }
        data.bb.putInt(m.tryStart);
        data.bb.putShort((short) (m.tryEnd - m.tryStart));
        data.bb.putShort((short) 0); // handler offset zero inside the handler list
        data.uleb(1); // one typed handler, no catch-all
        data.uleb(types.get(m.handlerType));
        data.uleb(m.handlerAddr);
        data.align(4);
    }

    private static void putString(Map<String, Integer> strings, String s) {
        if (!strings.containsKey(s)) {
            strings.put(s, strings.size());
        }
    }

    static byte[] zipWith(Map<String, byte[]> entries) throws Exception {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        try (ZipOutputStream zos = new ZipOutputStream(bos)) {
            for (Map.Entry<String, byte[]> en : entries.entrySet()) {
                zos.putNextEntry(new ZipEntry(en.getKey()));
                zos.write(en.getValue());
                zos.closeEntry();
            }
        }
        return bos.toByteArray();
    }

    // ---------------------------------------------------------------
    // Regression groups
    // ---------------------------------------------------------------

    public static void main(String[] args) throws Exception {
        List<MethodSpec> specs = buildSpecs();
        byte[] ordinary = buildImage(false, specs);
        byte[] compactImg = buildImage(true, specs);

        Events ordinaryEvents = collect(ordinary);
        Events compactEvents = collect(compactImg);

        // ---- code group: compact header, extension fields, exception ranges, instructions
        Check.group("code");
        Check.check(compactEvents.events.equals(ordinaryEvents.events),
                "compact and ordinary visitor events differ: " + diff(ordinaryEvents.events, compactEvents.events));
        Check.check(compactEvents.events.contains("REGISTERS 2"),
                "extension fields must yield two total registers (local + delta)");
        Check.check(compactEvents.events.contains("CONST const/4 0 7"), "const/4 instruction not visited");
        Check.check(compactEvents.events.contains("STMT1R return 0"), "return instruction not visited");
        List<String> tryEvents = filter(compactEvents.events, "TRY ");
        Check.check(tryEvents.size() == 1, "expected exactly one try range, got " + tryEvents);
        Check.check(tryEvents.get(0).contains("[Ljava/lang/RuntimeException;"),
                "handler type missing from try event: " + tryEvents.get(0));
        Check.check(tryEvents.get(0).startsWith("TRY L0000 L0001"),
                "try boundaries must use code-unit labels: " + tryEvents.get(0));
        Check.check(filter(ordinaryEvents.events, "TRY ").equals(tryEvents),
                "ordinary and compact try events must match");
        Check.check(count(compactEvents.events, "METHOD ") == specs.size(),
                "not all methods visited: " + count(compactEvents.events, "METHOD "));

        // ---- debug group: sparse table across the 16-entry block boundary, no fabricated lines
        Check.group("debug");
        Check.check(compactEvents.linesByMethod.get("m0").equals(Arrays.asList(42)), "m0 lines");
        Check.check(compactEvents.linesByMethod.get("m15").equals(Arrays.asList(43)),
                "m15 (last slot of first block) lines");
        Check.check(compactEvents.linesByMethod.get("m16").equals(Arrays.asList(44)),
                "m16 (first slot of second block) lines");
        Check.check(compactEvents.linesByMethod.get("tryMethod").equals(Arrays.asList(10, 12)),
                "tryMethod lines: " + compactEvents.linesByMethod.get("tryMethod"));
        Check.check(compactEvents.debugVisitsByMethod.get("m12") == 0
                && compactEvents.linesByMethod.get("m12").isEmpty(),
                "method without debug info must not receive a debug visitor or fabricated lines");
        Check.check(compactEvents.debugVisitsByMethod.get("m7") == 0, "absent debug entry must stay absent");
        int totalLines = 0;
        for (List<Integer> ls : compactEvents.linesByMethod.values()) {
            totalLines += ls.size();
        }
        Check.check(totalLines == 5, "exactly five line events expected, got " + totalLines);
        // Repeated visits must re-resolve debug info per method, never borrow another method's.
        Events repeat = new Events();
        repeat.accept(new DexFileReader(compactImg), 0);
        repeat.accept(new DexFileReader(compactImg), 0);
        Check.check(repeat.linesByMethod.get("m0").equals(Arrays.asList(42, 42)),
                "repeated visits must re-resolve m0 debug info");
        Check.check(repeat.linesByMethod.get("m12").isEmpty(),
                "repeated visits must not fabricate lines for m12");
        Events skipDebug = new Events();
        skipDebug.accept(new DexFileReader(compactImg), DexFileReader.SKIP_DEBUG);
        Check.check(filter(skipDebug.events, "LINE ").isEmpty(), "SKIP_DEBUG must suppress line events");
        Check.check(filter(skipDebug.events, "REGISTERS ").size() == specs.size(),
                "SKIP_DEBUG must keep code visiting");

        // ---- integration group: all entries, ByteBuffer state, ZIP, skip-then-reread
        Check.group("integration");
        Check.check(new DexFileReader(compactImg).getClassNames().equals(new DexFileReader(ordinary).getClassNames()),
                "byte[] entry class names");
        ByteBuffer caller = ByteBuffer.wrap(compactImg);
        caller.position(7);
        caller.limit(compactImg.length);
        Events bbEvents = new Events();
        bbEvents.accept(new DexFileReader(caller), 0);
        Check.check(caller.position() == 7 && caller.limit() == compactImg.length,
                "caller ByteBuffer position/limit must be preserved");
        Check.check(bbEvents.events.equals(ordinaryEvents.events), "ByteBuffer entry events");
        Path tmp = Files.createTempFile("compact-dex-regression", ".cdex");
        Files.write(tmp, compactImg);
        Events fileEvents = new Events();
        fileEvents.accept(new DexFileReader(tmp), 0);
        Files.delete(tmp);
        Check.check(fileEvents.events.equals(ordinaryEvents.events), "file entry events");
        BaseDexFileReader opened = MultiDexFileReader.open(compactImg);
        Events openEvents = new Events();
        openEvents.accept(opened, 0);
        Check.check(openEvents.events.equals(ordinaryEvents.events), "MultiDexFileReader.open(cdex) events");
        byte[] zip = zipWith(singleEntry("classes.dex", compactImg));
        Events zipEvents = new Events();
        zipEvents.accept(MultiDexFileReader.open(zip), 0);
        Check.check(zipEvents.events.equals(ordinaryEvents.events), "zip classes.dex cdex events");
        Map<String, byte[]> two = new LinkedHashMap<>();
        two.put("classes.dex", ordinary);
        two.put("classes2.dex", compactImg);
        BaseDexFileReader twoReader = MultiDexFileReader.open(zipWith(two));
        Check.check(twoReader.getClassNames().contains("LA;"), "multi-entry zip must expose the class");
        Events twoEvents = new Events();
        twoEvents.accept(twoReader, 0);
        Check.check(count(twoEvents.events, "CLASS ") == 1, "duplicate class across entries is de-duplicated");
        Events skipped = new Events();
        skipped.accept(new DexFileReader(compactImg), DexFileReader.SKIP_CODE);
        Check.check(filter(skipped.events, "REGISTERS ").isEmpty()
                && filter(skipped.events, "CONST ").isEmpty(), "SKIP_CODE must suppress code events");
        Events fullAfterSkip = new Events();
        fullAfterSkip.accept(new DexFileReader(compactImg), 0);
        Check.check(fullAfterSkip.events.equals(ordinaryEvents.events),
                "full visit after SKIP_CODE must recover every event");
        // Out-of-order, repeated single-class access keeps per-method debug isolation.
        DexFileReader rereader = new DexFileReader(compactImg);
        Events shuffled = new Events();
        shuffled.accept(rereader, 0, 0);
        shuffled.accept(rereader, 0, DexFileReader.SKIP_CODE);
        shuffled.accept(rereader, 0, 0);
        Check.check(shuffled.linesByMethod.get("m16").equals(Arrays.asList(44, 44)),
                "cross-method re-entry must keep m16 debug info isolated");
        Check.check(shuffled.linesByMethod.get("m12") == null || shuffled.linesByMethod.get("m12").isEmpty(),
                "cross-method re-entry must not invent m12 lines");
    }

    static Map<String, byte[]> singleEntry(String name, byte[] content) {
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put(name, content);
        return m;
    }

    static List<String> filter(List<String> in, String prefix) {
        List<String> out = new ArrayList<>();
        for (String s : in) {
            if (s.startsWith(prefix)) {
                out.add(s);
            }
        }
        return out;
    }

    static int count(List<String> in, String prefix) {
        return filter(in, prefix).size();
    }

    static String diff(List<String> a, List<String> b) {
        for (int i = 0; i < Math.max(a.size(), b.size()); i++) {
            String x = i < a.size() ? a.get(i) : "<missing>";
            String y = i < b.size() ? b.get(i) : "<missing>";
            if (!x.equals(y)) {
                return "first difference at " + i + ": ordinary=" + x + " compact=" + y;
            }
        }
        return "identical";
    }
}
