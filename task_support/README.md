Support uses only a JDK (17+); neither Gradle nor Maven is invoked.
The fixed runner compiles both upstream production source modules on every run.
PrepareOld adapts JUnit discovery only in a temporary build copy of the original
AnnotationTests.java. All three original fixture definitions, test bodies and
expected outputs are retained. OldAnnotationHarness implements the original
DecompilerTest.verifyOutput whitespace/import normalization with AssertionError,
and calls all three original tests. No JUnit compatibility stubs are supplied.
Original test files and licensing remain unchanged.

TaskTypeAnnotations.java is intentionally absent. Future tests register Case
instances through TaskCheck and call TaskCheck.check in every executed case.
The shell checks completion records outside the target JVM, including all three
groups; exiting the JVM early with status 0 does not count as completion.
Fixture.java and Baseline.java only witness existing behavior and contain no
type-path decoder, annotation AST reconstruction, or target contract tests.
