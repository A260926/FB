import { spawnSync } from "node:child_process";
import {
    cpSync,
    mkdtempSync,
    readFileSync,
    readdirSync,
    rmSync,
    symlinkSync,
    writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { run } from "node:test";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, "..");
const TEST_FILE = join(REPO_ROOT, "test", "stream-contract.mjs");
const TSC = join(REPO_ROOT, "harness", "node_modules", ".bin", "tsc");
const HARNESS_TSCONFIG = join(REPO_ROOT, "harness", "tsconfig.json");
const ROOT_TSCONFIG = join(REPO_ROOT, "tsconfig.json");

const MARKERS = {
    normal: "STREAM_MODEL_OK",
    attribute: "STREAM_ATTRIBUTE_CAUGHT",
    range: "STREAM_RANGE_CAUGHT",
    end: "STREAM_END_CAUGHT",
};

function markerFor(id) {
    const entry = DEGRADATIONS.find((candidate) => candidate.id === id);
    return entry === undefined ? "STREAM_UNKNOWN" : entry.marker;
}

function log(line) {
    process.stdout.write(`${line}\n`);
}

function runShell(command, arguments_, options = {}) {
    const result = spawnSync(command, arguments_, {
        cwd: options.cwd ?? REPO_ROOT,
        encoding: "utf8",
        env: options.env ?? process.env,
        // The detection step only accepts compiler/import/runtime failures
        // when a real assertion failed first, so capture everything.
        stdio: ["ignore", "pipe", "pipe"],
        timeout: options.timeout ?? 180_000,
    });
    return {
        status: result.status,
        stdout: result.stdout ?? "",
        stderr: result.stderr ?? "",
        error: result.error,
    };
}

// ---------------------------------------------------------------------------
// Normal build
// ---------------------------------------------------------------------------
function buildNormal() {
    const build = runShell(TSC, ["-p", HARNESS_TSCONFIG]);
    if (build.status !== 0) {
        log("STREAM_BUILD_FAILED");
        log(build.stdout.trim());
        log(build.stderr.trim());
        return false;
    }
    return true;
}

async function runMainTestFile(file, distEntry) {
    // `node:test`'s programmatic API emits the failure details we need; the
    // stream keeps the per-page subtests and their assertion messages.
    const failures = [];
    let summary = null;
    let ended = false;

    // The main test file resolves its build from this variable. Pass it to
    // the child test process so the normal build and each degraded build are
    // exercised against exactly the same test code.
    const stream = run({
        files: [file],
        concurrency: 1,
        testNamePatterns: /^stream acquisition contract$/,
        env: { ...process.env, STREAM_CONTRACT_DIST: distEntry },
    });

    stream.on("test:fail", (event) => {
        const error = event.details?.error;
        const cause = error?.cause;
        failures.push({
            name: event.name,
            nesting: event.nesting,
            type: event.details?.type,
            failureType: cause?.failureType ?? error?.failureType,
            code: cause?.code ?? error?.code,
            message: String(
                cause?.message ?? error?.message ?? "unknown failure",
            ),
        });
    });
    stream.on("test:summary", (event) => {
        // The runner reports one summary per file and one for the run; keep
        // the final aggregate, which carries the largest test count.
        if (summary === null || event.counts.tests > summary.counts.tests) {
            summary = {
                success: event.success,
                counts: { ...event.counts },
            };
        }
    });

    // One `test:summary` event per file plus one per run is emitted as the
    // stream finishes. The run is complete once that event has been seen
    // with the full test count; do not depend on `end`/`close`, which some
    // Node releases keep open behind the reported stream handle.
    await new Promise((resolvePromise) => {
        let received = 0;
        const finish = () => {
            if (ended) return;
            ended = true;
            // Let any trailing summary events land before resolving.
            setTimeout(resolvePromise, 25);
        };
        stream.on("test:summary", () => {
            received++;
            if (received >= 2) finish();
        });
        stream.on("error", finish);
        // Deterministic upper bound for a stuck runner; assertions still
        // come from the reported test count, never from this timer. The
        // handle is cleared on completion so it cannot keep the process
        // alive.
        const deadline = setTimeout(finish, 60_000);
        deadline.unref();
        stream.once("test:summary", () => clearTimeout(deadline));
    });

    return { failures, summary };
}

function summarize(failures, summary) {
    const assertionFailures = failures.filter(
        (failure) =>
            failure.failureType === "testCodeFailure" ||
            failure.code === "ERR_ASSERTION",
    );
    // The parent test fails with `subtestsFailed` precisely because a child
    // assertion failed; that is a consequence, not a crash.
    const crashFailures = failures.filter(
        (failure) =>
            !assertionFailures.includes(failure) &&
            failure.failureType !== "subtestsFailed",
    );
    return { assertionFailures, crashFailures, summary };
}

// ---------------------------------------------------------------------------
// Degradations. Each entry patches a copy of `src`, rebuilds the copy, and
// runs the same test file against the rebuilt entry point.
// ---------------------------------------------------------------------------
const DEGRADATIONS = [
    {
        id: "attribute-legacy",
        marker: MARKERS.attribute,
        description:
            "attributes decode in legacy text mode instead of attribute mode",
        modify(sourceDir) {
            const file = join(sourceDir, "Tokenizer.ts");
            const original = readFileSync(file, "utf8");
            const pattern =
                "                  ? DecodingMode.Legacy\n                  : DecodingMode.Attribute,";
            if (!original.includes(pattern)) {
                return { changed: false, file, hits: 0 };
            }
            // The replacement still compiles and imports; it only swaps the
            // decoding mode used while reading attribute values.
            const patched = original.replace(
                pattern,
                "                  ? DecodingMode.Legacy\n                  : DecodingMode.Legacy,",
            );
            writeFileSync(file, patched);
            return { changed: true, file, hits: 1 };
        },
    },
    {
        id: "text-end-range",
        marker: MARKERS.range,
        description:
            "entity text callbacks report one extra unit of source range",
        modify(sourceDir) {
            const file = join(sourceDir, "Parser.ts");
            const original = readFileSync(file, "utf8");
            const pattern =
                "    ontextentity(cp: number, endIndex: number): void {\n        this.endIndex = endIndex - 1;";
            if (!original.includes(pattern)) {
                return { changed: false, file, hits: 0 };
            }
            const patched = original.replace(
                pattern,
                "    ontextentity(cp: number, endIndex: number): void {\n        this.endIndex = endIndex;",
            );
            writeFileSync(file, patched);
            return { changed: true, file, hits: 1 };
        },
    },
    {
        id: "resume-drops-end",
        marker: MARKERS.end,
        description: "resume no longer delivers an end queued while paused",
        modify(sourceDir) {
            const file = join(sourceDir, "Parser.ts");
            const original = readFileSync(file, "utf8");
            const pattern =
                "        if (this.ended) this.tokenizer.end();\n    }\n}";
            if (!original.includes(pattern)) {
                return { changed: false, file, hits: 0 };
            }
            const patched = original.replace(
                pattern,
                "        if (this.ended && false) this.tokenizer.end();\n    }\n}",
            );
            writeFileSync(file, patched);
            return { changed: true, file, hits: 1 };
        },
    },
];

function linkDependencies(targetDir) {
    const target = join(targetDir, "node_modules");
    try {
        symlinkSync(join(REPO_ROOT, "node_modules"), target);
        return;
    } catch (error) {
        if (error?.code === "EEXIST") return;
    }
    // Fall back to copying only the runtime packages when symlinks are
    // unavailable.
    for (const name of [
        "domelementtype",
        "domhandler",
        "dom-serializer",
        "domutils",
        "entities",
    ]) {
        cpSync(join(REPO_ROOT, "node_modules", name), join(target, name), {
            recursive: true,
        });
    }
}

function copySources(targetDir) {
    // The temporary tree is compiled from its own copy of `src` and its own
    // package.json, so the module format and dependency resolution match the
    // real build.
    linkDependencies(targetDir);
    cpSync(join(REPO_ROOT, "package.json"), join(targetDir, "package.json"));
    cpSync(join(REPO_ROOT, "src"), join(targetDir, "src"), {
        recursive: true,
    });
    // Only the runtime sources are compiled; specs stay out of the build.
    for (const entry of readdirSync(join(targetDir, "src"))) {
        if (entry.endsWith(".spec.ts")) {
            rmSync(join(targetDir, "src", entry));
        }
    }
    return join(targetDir, "src");
}

function tscFor(targetDir) {
    const config = join(targetDir, "tsconfig.json");
    writeFileSync(
        config,
        JSON.stringify(
            {
                extends: ROOT_TSCONFIG,
                compilerOptions: {
                    outDir: "dist",
                    rootDir: "src",
                    // A throwaway build does not need declarations; emitting
                    // them would only constrain the degraded sources further.
                    declaration: false,
                    declarationMap: false,
                    sourceMap: false,
                    isolatedDeclarations: false,
                    typeRoots: [
                        join(REPO_ROOT, "harness", "node_modules", "@types"),
                    ],
                },
                include: ["src"],
            },
            null,
            4,
        ),
    );
    return runShell(TSC, ["-p", config], {
        cwd: targetDir,
        timeout: 240_000,
    });
}

async function verifyDegradation(degradation, rootDir) {
    const workDir = mkdtempSync(join(rootDir, `${degradation.id}-`));
    try {
        const sourceDir = copySources(workDir);
        const modification = degradation.modify(sourceDir);
        if (!modification.changed) {
            log(
                `STREAM_MUTATION\t${degradation.id}\tpatched=none\thits=0\tassertion-failures=0\tstatus=anchor-missing`,
            );
            return { id: degradation.id, ok: false, reason: "anchor-missing" };
        }

        // Compile the modified copy with the same repository compiler and
        // options as the normal build.
        const build = tscFor(workDir);
        if (build.status !== 0) {
            log(
                `STREAM_MUTATION\t${degradation.id}\tpatched=${relative(workDir, modification.file)}\thits=${modification.hits}\tassertion-failures=0\tstatus=build-failed`,
            );
            log(build.stdout.trim().slice(0, 2000));
            log(build.stderr.trim().slice(0, 2000));
            return { id: degradation.id, ok: false, reason: "build-failed" };
        }

        // Report paths relative to the temporary root so the log does not
        // depend on the host's temp directory layout.
        const tempPath = relative(workDir, modification.file);
        const distEntry = join(workDir, "dist", "index.js");
        try {
            await import(pathToFileURL(distEntry).href);
        } catch (error) {
            log(
                `STREAM_MUTATION\t${degradation.id}\tpatched=${tempPath}\thits=${modification.hits}\tassertion-failures=0\tstatus=import-failed`,
            );
            log(String(error?.stack ?? error));
            return { id: degradation.id, ok: false, reason: "import-failed" };
        }

        const result = await runMainTestFile(TEST_FILE, distEntry);
        const { assertionFailures, crashFailures, summary } = summarize(
            result.failures,
            result.summary,
        );

        if (summary === null || summary.counts.tests === 0) {
            log(
                `STREAM_MUTATION\t${degradation.id}\tpatched=${tempPath}\thits=${modification.hits}\tassertion-failures=0\tstatus=no-tests`,
            );
            return { id: degradation.id, ok: false, reason: "no-tests" };
        }
        if (crashFailures.length > 0) {
            log(
                `STREAM_MUTATION\t${degradation.id}\tpatched=${tempPath}\thits=${modification.hits}\tassertion-failures=${assertionFailures.length}\tstatus=crash`,
            );
            for (const failure of crashFailures) {
                log(`\t${failure.name}: ${failure.message.split("\n")[0]}`);
            }
            return {
                id: degradation.id,
                ok: false,
                reason: "crash",
                failures: crashFailures,
            };
        }
        if (assertionFailures.length === 0) {
            log(
                `STREAM_MUTATION\t${degradation.id}\tpatched=${tempPath}\thits=${modification.hits}\tassertion-failures=0\tstatus=not-caught`,
            );
            return { id: degradation.id, ok: false, reason: "not-caught" };
        }

        // Name the failing cases by aspect so the log is actionable.
        const failingCases = assertionFailures
            .map((failure) => failure.name)
            .filter((name, index, names) => names.indexOf(name) === index);
        log(
            [
                "STREAM_MUTATION",
                degradation.id,
                `patched=${tempPath}`,
                `hits=${modification.hits}`,
                `assertion-failures=${assertionFailures.length}`,
                "status=caught",
                `examples=${failingCases.slice(0, 6).join(",")}`,
            ].join("\t"),
        );
        log(degradation.marker);
        return {
            id: degradation.id,
            ok: true,
            reason: "caught",
            marker: degradation.marker,
            failures: assertionFailures,
            counts: summary.counts,
        };
    } finally {
        rmSync(workDir, { recursive: true, force: true });
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
async function main() {
    if (!buildNormal()) {
        process.exitCode = 1;
        return;
    }

    const normalDist = join(REPO_ROOT, "dist", "index.js");
    const normal = await runMainTestFile(TEST_FILE, normalDist);
    const normalSummary = summarize(normal.failures, normal.summary);
    if (
        normalSummary.summary === null ||
        normalSummary.summary.counts.tests === 0
    ) {
        log("STREAM_MODEL_FAILED\tno normal tests ran");
        process.exitCode = 1;
        return;
    }
    for (const key of ["failed", "cancelled", "skipped", "todo"]) {
        if (normalSummary.summary.counts[key] !== 0) {
            log(`STREAM_MODEL_FAILED\t${key}=${normalSummary.summary.counts[key]}`);
            process.exitCode = 1;
            return;
        }
    }
    if (
        !normalSummary.summary.success ||
        normalSummary.summary.counts.passed === 0 ||
        normal.failures.length > 0
    ) {
        log("STREAM_MODEL_FAILED");
        for (const failure of normal.failures) {
            log(`\t${failure.name}: ${failure.message.split("\n")[0]}`);
        }
        process.exitCode = 1;
        return;
    }
    if (normalSummary.summary.counts.passed !== normalSummary.summary.counts.tests) {
        log("STREAM_MODEL_FAILED\tnot all normal tests passed");
        process.exitCode = 1;
        return;
    }

    log(MARKERS.normal);

    const rootDir = mkdtempSync(join(tmpdir(), "stream-contract-"));
    try {
        const results = [];
        for (const degradation of DEGRADATIONS) {
            results.push(await verifyDegradation(degradation, rootDir));
        }
        const missing = results.filter((result) => !result.ok);
        if (missing.length > 0) {
            for (const result of missing) {
                log(
                    `${markerFor(result.id)}\tMISSING\t${result.id}\t${result.reason}`,
                );
            }
            process.exitCode = 1;
            return;
        }
    } finally {
        rmSync(rootDir, { recursive: true, force: true });
    }
}

await main();
