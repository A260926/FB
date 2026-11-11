#!/usr/bin/env node
/**
 * @file Mutation check for the DOM XSS flow regression tests.
 *
 * Verifies that tests/regress/domxss-flow.test.cjs actually detects three
 * targeted degradations of the no-unsanitized rules:
 *
 *  1. ignore-later-writes:    writing references to a binding are no longer
 *                             inspected, so a tainted write after a safe
 *                             initializer goes unnoticed.
 *  2. either-side-safe-concat: string concatenation is allowed when only
 *                             one side is safe instead of requiring both.
 *  3. position-arg-as-html:   insertAdjacentHTML checks its position
 *                             argument (index 0) instead of the HTML
 *                             argument (index 1).
 *
 * For each mutant the script copies the plugin sources into a temporary
 * directory, applies exactly one source-level mutation, proves the copy
 * still parses and loads, and then runs the regression test file against
 * it via DOMXSS_PLUGIN_ROOT. A mutant counts as detected only when the
 * test run executes the same number of tests as the baseline run and
 * fails. Syntax errors, unloadable copies and empty test runs are script
 * failures, never detections. The repository sources are hashed before
 * and after and must be byte-identical.
 *
 * Exit code is 0 only when the baseline passes and every mutant is
 * detected.
 */
"use strict";

const { spawnSync } = require("node:child_process");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");

const REPO_ROOT = path.join(__dirname, "..");
const TEST_FILE = path.join(REPO_ROOT, "tests", "regress", "domxss-flow.test.cjs");
const PLUGIN_FILES = [
    "index.js",
    "package.json",
    "lib/ruleHelper.js",
    "lib/rules/property.js",
    "lib/rules/method.js",
];

const MUTANTS = [
    {
        name: "ignore-later-writes",
        file: "lib/ruleHelper.js",
        find: "if (ref.isWrite()) {",
        replace: "if (false) { // MUTANT: ignore-later-writes",
    },
    {
        name: "either-side-safe-concat",
        file: "lib/ruleHelper.js",
        find: "                    ) &&\n                    this.allowedExpression(",
        replace: "                    ) || // MUTANT: either-side-safe-concat\n                    this.allowedExpression(",
    },
    {
        name: "position-arg-as-html",
        file: "lib/rules/method.js",
        find: "    insertAdjacentHTML: {\n        properties: [1],",
        replace: "    insertAdjacentHTML: {\n        properties: [0], // MUTANT: position-arg-as-html",
    },
];

/**
 * SHA-256 of every plugin source file, keyed by relative path.
 *
 * @returns {Map} Path to hex digest.
 */
function hashPluginFiles() {
    const hashes = new Map();
    for (const rel of PLUGIN_FILES) {
        const content = fs.readFileSync(path.join(REPO_ROOT, rel));
        hashes.set(rel, crypto.createHash("sha256").update(content).digest("hex"));
    }
    return hashes;
}

/**
 * Run the regression test file, optionally against a mutated plugin copy.
 *
 * @param {string|null} pluginRoot Directory of the plugin copy, or null
 *   for the repository sources.
 * @returns {object} The spawn result.
 */
function runTests(pluginRoot) {
    const env = { ...process.env };
    if (pluginRoot) {
        env.DOMXSS_PLUGIN_ROOT = pluginRoot;
    } else {
        delete env.DOMXSS_PLUGIN_ROOT;
    }
    return spawnSync(
        process.execPath,
        ["--test", "--test-reporter=tap", TEST_FILE],
        { cwd: REPO_ROOT, env, encoding: "utf8" }
    );
}

/**
 * Extract test counts and failing test names from TAP output.
 *
 * @param {string} tap TAP reporter output.
 * @returns {object} Parsed totals, failing test names and diagnostics.
 */
function parseTap(tap) {
    const total = /# tests (\d+)/.exec(tap);
    const failed = /# fail (\d+)/.exec(tap);
    const failing = [];
    const diagnostics = [];
    const lines = tap.split("\n");
    for (let i = 0; i < lines.length; i++) {
        const notOk = /^\s*not ok \d+ - (.+)$/.exec(lines[i]);
        if (notOk) {
            failing.push(notOk[1]);
            // Collect the YAML diagnostic block belonging to this failure.
            const block = [];
            for (let j = i + 1; j < lines.length; j++) {
                if (/^\s*(not )?ok \d+ - /.test(lines[j]) || lines[j] === "...") {
                    break;
                }
                if (/^\s+(error|expected|actual|code|failureType):/.test(lines[j])) {
                    block.push(lines[j].trim());
                }
            }
            if (block.length > 0) {
                diagnostics.push(`${notOk[1]}:\n    ${block.join("\n    ")}`);
            }
        }
    }
    return {
        tests: total ? Number(total[1]) : null,
        fail: failed ? Number(failed[1]) : null,
        failing,
        diagnostics,
    };
}

/**
 * Create a temporary copy of the plugin with one mutation applied.
 *
 * @param {object} mutant Mutant descriptor.
 * @param {string} dir Directory to populate.
 * @returns {string} The directory of the mutated copy.
 */
function makeMutantCopy(mutant, dir) {
    for (const rel of PLUGIN_FILES) {
        const target = path.join(dir, rel);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.copyFileSync(path.join(REPO_ROOT, rel), target);
    }
    const target = path.join(dir, mutant.file);
    const source = fs.readFileSync(target, "utf8");
    const occurrences = source.split(mutant.find).length - 1;
    if (occurrences !== 1) {
        throw new Error(
            `mutant ${mutant.name}: anchor occurs ${occurrences} time(s) in ${mutant.file}, expected exactly 1`
        );
    }
    const mutated = source.replace(mutant.find, mutant.replace);
    if (mutated === source) {
        throw new Error(`mutant ${mutant.name}: mutation did not change ${mutant.file}`);
    }
    fs.writeFileSync(target, mutated);
    return dir;
}

/**
 * Prove that a plugin copy still parses and loads as a module.
 *
 * @param {string} dir Directory of the plugin copy.
 */
function assertLoadable(dir) {
    for (const rel of PLUGIN_FILES.filter((f) => f.endsWith(".js"))) {
        // Parse check: compiles the file without executing it.
        new vm.Script(fs.readFileSync(path.join(dir, rel), "utf8"), {
            filename: rel,
        });
    }
    const copy = require(path.join(dir, "index.js"));
    if (
        typeof copy.rules?.property?.create !== "function" ||
        typeof copy.rules?.method?.create !== "function"
    ) {
        throw new Error("mutated copy does not expose both rules");
    }
}

function main() {
    const before = hashPluginFiles();
    const failures = [];
    const tempDirs = [];

    try {
        // 1. Baseline: the regression tests must pass on the original code.
        console.log("== baseline: original sources ==");
        const baseline = runTests(null);
        const baselineTap = parseTap(baseline.stdout || "");
        if (baseline.status !== 0 || baselineTap.fail !== 0) {
            console.error(baseline.stdout);
            console.error(baseline.stderr);
            throw new Error("baseline test run failed on unmodified sources");
        }
        if (!baselineTap.tests || baselineTap.tests < 3) {
            throw new Error(
                `baseline run reported ${baselineTap.tests} tests; refusing to compare against an empty run`
            );
        }
        console.log(`baseline ok: ${baselineTap.tests} tests, 0 failures`);

        // 2. Each mutant must be caught by the regression tests.
        for (const mutant of MUTANTS) {
            console.log(`\n== mutant: ${mutant.name} ==`);
            const dir = fs.mkdtempSync(
                path.join(os.tmpdir(), `domxss-mutant-${mutant.name}-`)
            );
            tempDirs.push(dir);
            makeMutantCopy(mutant, dir);
            assertLoadable(dir);
            console.log("mutated copy parses and loads");

            const result = runTests(dir);
            const tap = parseTap(result.stdout || "");

            const detected =
                result.status !== 0 &&
                tap.tests === baselineTap.tests &&
                tap.fail > 0 &&
                tap.failing.length > 0;

            if (!detected) {
                failures.push(mutant.name);
                console.log(`ESCAPED or INVALID: ${mutant.name}`);
                console.log(
                    `exit=${result.status} tests=${tap.tests} (baseline ${baselineTap.tests}) fail=${tap.fail}`
                );
                if (tap.tests !== baselineTap.tests) {
                    console.log(
                        "test run did not execute the baseline test set; not a valid detection"
                    );
                    console.log(result.stderr || "");
                }
                continue;
            }

            console.log(`DETECTED: ${mutant.name}`);
            console.log("failing tests:");
            for (const name of tap.failing) {
                console.log(`  - ${name}`);
            }
            console.log("diagnostics:");
            for (const diag of tap.diagnostics) {
                console.log(`  ${diag}`);
            }
        }
    } finally {
        for (const dir of tempDirs) {
            fs.rmSync(dir, { recursive: true, force: true });
        }
    }

    // 3. The repository sources must be untouched.
    const after = hashPluginFiles();
    for (const [rel, hash] of before) {
        if (after.get(rel) !== hash) {
            failures.push(`source-modified:${rel}`);
            console.error(`source file changed during the run: ${rel}`);
        }
    }

    if (failures.length > 0) {
        console.error(`\nFAIL: ${failures.join(", ")}`);
        process.exit(1);
    }
    console.log(`\nOK: all ${MUTANTS.length} mutants detected, sources unchanged`);
}

main();
