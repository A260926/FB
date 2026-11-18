package task_support;

import java.nio.file.*;
import org.jd.core.v1.api.loader.*;
import org.jd.core.v1.model.classfile.ClassFile;
import org.jd.core.v1.model.classfile.Method;
import org.jd.core.v1.model.classfile.attribute.AttributeCode;
import org.jd.core.v1.model.message.Message;
import org.jd.core.v1.service.deserializer.classfile.DeserializeClassFileProcessor;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.cfg.BasicBlock;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.cfg.ControlFlowGraph;
import org.jd.core.v1.service.converter.classfiletojavasyntax.util.*;

/**
 * Debug-only probe: deserialize a class with the production loader/deserializer
 * and print the reduced control flow graph of each method.
 */
public final class CfgDump {
    public static void main(String[] args) throws Exception {
        final Path root = Paths.get(args[0]);
        Loader loader = new Loader() {
            public boolean canLoad(String name) { return Files.isRegularFile(root.resolve(name + ".class")); }
            public byte[] load(String name) throws LoaderException {
                try { return Files.readAllBytes(root.resolve(name + ".class")); }
                catch (java.io.IOException e) { throw new LoaderException(e.toString()); }
            }
        };
        Message message = new Message();
        message.setHeader("mainInternalTypeName", args[1]);
        message.setHeader("loader", loader);
        new DeserializeClassFileProcessor().process(message);
        ClassFile classFile = message.getBody();

        for (Method method : classFile.getMethods()) {
            AttributeCode code = method.getAttribute("Code");
            if (code == null) continue;
            System.out.println("=== " + method.getName() + method.getDescriptor());
            ControlFlowGraph cfg = ControlFlowGraphMaker.make(method);
            ControlFlowGraphGotoReducer.reduce(cfg);
            ControlFlowGraphLoopReducer.reduce(cfg);
            boolean reduced = ControlFlowGraphReducer.reduce(cfg);
            System.out.println("reduced=" + reduced);
            for (BasicBlock bb : cfg.getBasicBlocks()) {
                if (bb.getType() == BasicBlock.TYPE_DELETED) continue;
                StringBuilder sb = new StringBuilder();
                sb.append(' ').append(bb.getIndex()).append(": ").append(bb.getTypeName())
                  .append(" [").append(bb.getFromOffset()).append('-').append(bb.getToOffset()).append(']');
                if (bb.getNext() != BasicBlock.END) sb.append(" next=").append(safe(bb.getNext()));
                if (bb.getBranch() != BasicBlock.END) sb.append(" branch=").append(safe(bb.getBranch()));
                if (bb.getSub1() != BasicBlock.END) sb.append(" sub1=").append(safe(bb.getSub1()));
                if (bb.getSub2() != BasicBlock.END) sb.append(" sub2=").append(safe(bb.getSub2()));
                if (bb.getCondition() != BasicBlock.END) sb.append(" cond=").append(safe(bb.getCondition()));
                if (!bb.getSwitchCases().isEmpty()) {
                    sb.append(" cases={");
                    for (BasicBlock.SwitchCase sc : bb.getSwitchCases()) {
                        sb.append(sc.isDefaultCase() ? "default" : sc.getValue()).append("->").append(safe(sc.getBasicBlock())).append(' ');
                    }
                    sb.append('}');
                }
                System.out.println(sb);
            }
        }
    }

    static String safe(BasicBlock bb) {
        if (bb == null) return "NULL";
        if (bb == BasicBlock.END) return "END";
        if (bb == BasicBlock.SWITCH_BREAK) return "SWITCH_BREAK";
        if (bb == BasicBlock.LOOP_START) return "LOOP_START";
        if (bb == BasicBlock.LOOP_END) return "LOOP_END";
        if (bb == BasicBlock.LOOP_CONTINUE) return "LOOP_CONTINUE";
        if (bb == BasicBlock.RETURN) return "RETURN";
        return String.valueOf(bb.getIndex());
    }
}
