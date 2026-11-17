# Initialization tracking for the ASM tree analysis

This workspace adds an object-construction view on top of `tree.analysis`:

- `org.objectweb.asm.tree.analysis.InitializationValue` — an immutable `Value`
  with a `getKind()` (`TOP`, `VALUE`, `NEW`, `UNINITIALIZED_THIS`), a
  `getType()` (`null` for `TOP`), and a `getAllocation()` (the `TypeInsnNode`
  of the `NEW` instruction, `null` once initialized).
- `org.objectweb.asm.tree.analysis.InitializationInterpreter` — a no-arg
  interpreter; use it with the stock analyzer and frames:

  ```java
  Frame<InitializationValue>[] frames =
      new Analyzer<InitializationValue>(new InitializationInterpreter())
          .analyze(classNode.name, methodNode);
  ```

Semantics in short: a `NEW` instruction produces a `NEW` value whose identity
is the instruction itself (two `NEW`s of the same type never mix); `DUP`,
`ALOAD`, `ASTORE` propagate the same identity; the entry `this` of a
constructor is `UNINITIALIZED_THIS` of the own class. When an
`invokespecial <init>` returns normally, every live alias of the receiver on
the stack and in the locals becomes a `VALUE` of the allocated type (a super
constructor does not turn `this` into the super class); the exceptional edge
keeps the pre-call frame. Merges keep a shared uninitialized identity, turn
mixed or distinct identities into `TOP`, keep equal reference types, merge
`null` with a reference to the reference, and merge distinct reference types
to `java/lang/Object` without loading any class. Ordinary instruction types
and slot sizes follow the `BasicInterpreter` conventions.

Extension points (default behavior keeps `BasicInterpreter` and
`SourceInterpreter` unchanged):

- `Interpreter.newEntryValue(Type, String, MethodNode, int)` — initial value
  of a local variable of the analyzed method; defaults to `newValue(Type)`.
- `Interpreter.invokeOperation(AbstractInsnNode, List, Frame)` — interprets
  `INVOKE*` instructions with access to the frame after the arguments have
  been popped; defaults to `naryOperation`.
- `Frame.setStack(int, V)` — replaces an operand stack slot.

## Commands

```sh
bash task_support/run.sh baseline   # compile src + support, run task.Baseline
bash task_support/run.sh target     # also run the InitializationContract checks
java -ea -cp .task-build/classes task.InitializationDemo   # frames around <init>
```

`task_support/InitializationContract.java` registers the regression cases
(groups `aliases`, `flow`, `compatibility`); `task_support/InitializationDemo.java`
prints the frames on both sides of each constructor call.
