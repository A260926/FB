/**
 * Mutation runner for the rank contract in `checks/rank-contract.test.mjs`.
 *
 * Three independent, disposable source copies are created under the OS temp
 * directory. Each copy receives exactly one buildable behavior regression in
 * `src/pseudo-selectors/filters.ts`, is rebuilt with TypeScript, and then runs
 * the unchanged rank contract test against its own build:
 *
 *   1. CANDIDATE_GUARD  - the candidate no longer has to match the `of S` set
 *   2. REVERSE_RANK     - `:nth-last-child` numbers siblings from the start
 *   3. NON_ELEMENT      - positional rules without `of` count text/comments
 *
 * A regression only counts as "killed" when the pristine copy passes and the
 * mutated copy fails with a concrete test assertion. A missing module, a
 * syntax/type error, or a timeout are reported as failures, not kills. The
 * original source tree and the existing tests are never modified.
 *
 * Run with: `node checks/run-rank-mutants.mjs`
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    "..",
);
const filtersPath = path.join("src", "pseudo-selectors", "filters.ts");
const testRelativePath = path.join("checks", "rank-contract.test.mjs");

const TEST_TIMEOUT_MS = 60_000;

const MUTANTS = [
    {
        marker: "CANDIDATE_GUARD_KILLED",
        description:
            "drop the requirement that the candidate itself matches the of-selector",
        find: "            if (ofSelector && !ofSelector(element)) return false;",
        replace: "            if (ofSelector && false) return false;",
        expectedOccurrences: 2,
    },
    {
        marker: "REVERSE_RANK_KILLED",
        description:
            "make nth-last-child number siblings from the start instead of the end",
        find: "                for (let index = siblings.length - 1; index >= 0; index--) {",
        replace:
            "                for (let index = 0; index < siblings.length; index++) {",
        expectedOccurrences: 1,
    },
    {
        marker: "NON_ELEMENT_KILLED",
        description:
            "let positional rules without an of-list count text and comment nodes",
        // Identical on both the forward and reverse counting loops.
        find: "adapter.isTag(sibling) && shouldCount(element, sibling)",
        replace: "shouldCount(element, sibling as ElementNode)",
        expectedOccurrences: 2,
    },
];

function copyDirectory(source, destination) {
    fs.mkdirSync(destination, { recursive: true });
    const entries = fs.readdirSync(source, { withFileTypes: true });
    for (const entry of entries) {
        const sourcePath = path.join(source, entry.name);
        const destinationPath = path.join(destination, entry.name);
        if (entry.isDirectory()) {
            copyDirectory(sourcePath, destinationPath);
        } else {
            fs.copyFileSync(sourcePath, destinationPath);
        }
    }
}

function copyIntoTemporary(label) {
    const directory = fs.mkdtempSync(
        path.join(os.tmpdir(), `rank-mutant-${label}-`),
    );

    /*
     * Only the files the build and the test need; node_modules is symlinked
     * so the copies stay cheap and cannot pollute each other.
     */
    copyDirectory(path.join(repoRoot, "src"), path.join(directory, "src"));
    copyDirectory(
        path.join(repoRoot, "checks"),
        path.join(directory, "checks"),
    );
    fs.copyFileSync(
        path.join(repoRoot, "tsconfig.json"),
        path.join(directory, "tsconfig.json"),
    );
    fs.copyFileSync(
        path.join(repoRoot, "package.json"),
        path.join(directory, "package.json"),
    );
    fs.symlinkSync(
        path.join(repoRoot, "node_modules"),
        path.join(directory, "node_modules"),
        "dir",
    );

    return directory;
}

function run(command, arguments_, cwd, timeout = TEST_TIMEOUT_MS) {
    try {
        const output = execFileSync(command, arguments_, {
            cwd,
            timeout,
            encoding: "utf8",
            stdio: ["ignore", "pipe", "pipe"],
        });
        return { status: 0, output, timedOut: false };
    } catch (error) {
        return {
            status: typeof error.status === "number" ? error.status : 1,
            output: `${error.stdout ?? ""}${error.stderr ?? ""}`,
            timedOut: error.signal === "SIGTERM" || error.killed === true,
        };
    }
}

function buildCopy(directory) {
    const tsc = path.join(
        directory,
        "node_modules",
        "typescript",
        "bin",
        "tsc",
    );
    return run(process.execPath, [tsc, "-p", directory], directory);
}

function runContractTests(directory) {
    return run(
        process.execPath,
        ["--test", "--test-reporter=tap", path.join(directory, testRelativePath)],
        directory,
    );
}

const NOT_OK_LINE = /^(\s*)not ok \d+ - (.+?)\s*$/;

function failedCaseNames(output) {
    const names = [];
    for (const line of output.split(/\r?\n/)) {
        const match = NOT_OK_LINE.exec(line);
        if (match) names.push(match[2]);
    }
    return names;
}

function hasInfrastructureFailure(output) {
    return (
        output.includes("ERR_MODULE_NOT_FOUND") ||
        output.includes("Cannot find module") ||
        output.includes("SyntaxError") ||
        output.includes("ERR_SYNTAX") ||
        // TypeScript build diagnostics are reported as `error TSxxxx`.
        /\berror TS\d{4}\b/.test(output)
    );
}

function summarizeFailures(output) {
    const names = failedCaseNames(output);
    return names.length > 0 ? names.join(" | ") : "(no failing case named)";
}

const keptDirectories = [];

try {
    // The pristine copy proves the test itself is green before any mutation.
    const pristine = copyIntoTemporary("pristine");
    const pristineBuild = buildCopy(pristine);
    if (pristineBuild.status !== 0) {
        throw new Error(
            `pristine build failed at ${pristine}\n${pristineBuild.output}`,
        );
    }
    const pristineTest = runContractTests(pristine);
    if (pristineTest.status !== 0 || pristineTest.timedOut) {
        throw new Error(
            `pristine copy must pass, but it did not (${pristine})\n${pristineTest.output}`,
        );
    }
    console.log("PRISTINE_PASS rank-contract tests are green without mutation");

    let isAllKilled = true;

    for (const mutant of MUTANTS) {
        const directory = copyIntoTemporary(
            mutant.marker.replace(/_KILLED$/, "").toLowerCase(),
        );
        const sourceFile = path.join(directory, filtersPath);
        const original = fs.readFileSync(sourceFile, "utf8");
        const occurrences = original.split(mutant.find).length - 1;

        if (occurrences !== mutant.expectedOccurrences) {
            throw new Error(
                `${mutant.marker}: expected ${mutant.expectedOccurrences} mutation site(s), ` +
                    `found ${occurrences} in ${filtersPath}`,
            );
        }

        fs.writeFileSync(
            sourceFile,
            original.split(mutant.find).join(mutant.replace),
        );

        const build = buildCopy(directory);
        if (build.status !== 0) {
            isAllKilled = false;
            keptDirectories.push(directory);
            console.error(
                `${mutant.marker}: mutated copy failed to build (not counted as killed)\n${build.output}`,
            );
            continue;
        }

        const testRun = runContractTests(directory);

        const failedCases = failedCaseNames(testRun.output);
        const isConcreteAssertion =
            !testRun.timedOut &&
            testRun.status !== 0 &&
            failedCases.length > 0 &&
            !hasInfrastructureFailure(testRun.output);

        if (isConcreteAssertion) {
            console.log(
                `${mutant.marker} detectedBy=${summarizeFailures(testRun.output)}`,
            );
            console.log(mutant.marker);
        } else {
            isAllKilled = false;
            keptDirectories.push(directory);
            const reason = testRun.timedOut
                ? "test run timed out"
                : testRun.status === 0
                  ? "all tests still passed; mutation was not detected"
                  : "failure was not a concrete assertion";
            console.error(
                `${mutant.marker}: NOT killed (${reason}) in ${directory}`,
            );
            console.error(testRun.output);
        }
    }

    if (!isAllKilled) {
        process.exitCode = 1;
    }
} catch (error) {
    process.exitCode = 1;
    console.error(error instanceof Error ? error.stack ?? error.message : error);
} finally {
    if (process.exitCode) {
        console.error(
            `kept mutant copies for inspection: ${keptDirectories.join(", ")}`,
        );
    } else {
        const staleCopies = fs
            .readdirSync(os.tmpdir(), { withFileTypes: true })
            .filter((entry) => entry.name.startsWith("rank-mutant-"));
        for (const entry of staleCopies) {
            fs.rmSync(path.join(os.tmpdir(), entry.name), {
                recursive: true,
                force: true,
            });
        }
    }
}
