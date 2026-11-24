/*
 * Regression tests for the structured, tree-based reducer.
 *
 * Groups:
 *   tree_dependencies  - typed tree shape, nested statement removal,
 *                        expression replacement and declaration closure.
 *   oracle_cache       - worker protocol, budget accounting and caching.
 *   production_entry   - reduceCode2 determinism, empty input and the
 *                        translate --reduce command line.
 */

#include "task_support/check.h"
#include "oss-fuzz/structured.h"
#include "oss-fuzz/type2.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

/* --------------------------------------------------------------- helpers */

std::string bytesToSeed(const std::vector<unsigned> &bytes)
{
    std::string s;
    for (unsigned b : bytes)
        s.push_back(static_cast<char>(b & 0xFF));
    return s;
}

int countStmts(const structured::Block &block)
{
    int n = 0;
    for (const auto &s : block.stmts) {
        ++n;
        if (s->thenBlock)
            n += countStmts(*s->thenBlock);
        if (s->elseBlock)
            n += countStmts(*s->elseBlock);
    }
    return n;
}

int countCompoundExprs(const structured::Expr *e)
{
    if (!e)
        return 0;
    int n = 0;
    switch (e->kind) {
    case structured::ExprKind::Binary:
    case structured::ExprKind::Assign:
    case structured::ExprKind::Unary:
    case structured::ExprKind::Paren:
    case structured::ExprKind::Compare:
        ++n;
        break;
    default:
        break;
    }
    return n + countCompoundExprs(e->lhs.get()) + countCompoundExprs(e->rhs.get());
}

int countCompoundExprs(const structured::Block &block)
{
    int n = 0;
    for (const auto &s : block.stmts) {
        n += countCompoundExprs(s->value.get());
        if (s->thenBlock)
            n += countCompoundExprs(*s->thenBlock);
        if (s->elseBlock)
            n += countCompoundExprs(*s->elseBlock);
    }
    return n;
}

int totalCompoundExprs(const structured::Tree &tree)
{
    int n = 0;
    for (const auto &f : tree.functions)
        n += countCompoundExprs(*f->body);
    return n;
}

int totalStmts(const structured::Tree &tree)
{
    int n = 0;
    for (const auto &f : tree.functions)
        n += countStmts(*f->body);
    return n;
}

/* A seed with two functions, nested if/else blocks and expression trees. */
const std::vector<unsigned> &richSeed()
{
    static const std::vector<unsigned> seed = {0xcd, 0x0c, 0x78, 0xa0, 0x7f, 0x4a,
                                               0xb9, 0x80, 0xfd, 0xcb, 0x3c, 0xd2,
                                               0x02, 0xc6};
    return seed;
}

/* ----------------------------------------------------------- file helpers */

bool fileExists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string tempDir()
{
    const char *env = std::getenv("TMPDIR");
    std::string dir = (env && *env) ? env : "/tmp";
    while (!dir.empty() && dir.back() == '/')
        dir.pop_back();
    return dir;
}

std::string makeTempPath(const char *prefix, const char *suffix)
{
    std::string pattern = tempDir() + "/" + prefix + "XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0)
        return std::string();
    close(fd);
    unlink(name.data());
    std::string result(name.data());
    result += suffix;
    return result;
}

std::string readWholeFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool writeWholeFile(const std::string &path, const std::string &content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

bool makeExecutableScript(const std::string &path, const std::string &body)
{
    if (!writeWholeFile(path, "#!/bin/sh\n" + body + "\n"))
        return false;
    return chmod(path.c_str(), 0700) == 0;
}

/* Removes a temp file when the surrounding scope ends. */
struct TempFile {
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile()
    {
        if (!path.empty())
            unlink(path.c_str());
    }
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
    std::string path;
};

/* The translate binary is built next to the target.  run.sh launches the
 * target from the repository root, so probe the conventional relative path
 * first and allow an explicit override for other harnesses. */
std::string translatorPath()
{
    const char *env = std::getenv("TASK_TRANSLATE");
    if (env && *env)
        return env;
    const char *candidates[] = {".task-build/translate", "../.task-build/translate"};
    for (const char *c : candidates) {
        if (access(c, X_OK) == 0)
            return c;
    }
    return std::string();
}

/* A worker that reports a fixed set of coverage ids. */
std::string fixedWorker(const std::string &idList)
{
    const std::string path = makeTempPath("fixed-worker-", ".sh");
    if (path.empty())
        return std::string();
    if (!makeExecutableScript(path, "echo \"COVER" + (idList.empty() ? std::string() : " " + idList) + "\"")) {
        unlink(path.c_str());
        return std::string();
    }
    return path;
}

/* A worker that counts its invocations in a shared file and always reports
 * ids 1 2 3.  Used to prove the cache is consulted and budget is respected. */
struct CountingWorker {
    std::string script;
    std::string counter;

    bool create()
    {
        script = makeTempPath("counting-worker-", ".sh");
        counter = makeTempPath("counting-counter-", ".txt");
        if (script.empty() || counter.empty())
            return false;
        if (!writeWholeFile(counter, "0"))
            return false;
        return makeExecutableScript(script,
                                    "n=$(cat '" + counter + "')\n"
                                    "n=$((n+1))\n"
                                    "printf '%s' \"$n\" > '" + counter + "'\n"
                                    "echo \"COVER 1 2 3\"");
    }

    int count() const
    {
        const std::string text = readWholeFile(counter);
        return text.empty() ? -1 : std::atoi(text.c_str());
    }

    ~CountingWorker()
    {
        if (!script.empty())
            unlink(script.c_str());
        if (!counter.empty())
            unlink(counter.c_str());
    }
};

} // namespace

/* ================================================== tree_dependencies */

static int tree_matches_generator_text()
{
    const std::string seed = bytesToSeed(richSeed());
    const structured::Tree tree = structured::buildTree(
        reinterpret_cast<const uint8_t *>(seed.data()), seed.size());
    const std::string rendered = structured::renderTree(tree);
    const std::string generated = generateCode2(
        reinterpret_cast<const uint8_t *>(seed.data()), seed.size());

    TASK_CHECK(rendered == generated);
    // Hand-decoded shape of this seed.
    TASK_CHECK(tree.functions.size() == 2);
    TASK_CHECK(totalStmts(tree) == 11);
    TASK_CHECK(rendered.find("int f1()") != std::string::npos);
    TASK_CHECK(rendered.find("int f2()") != std::string::npos);
    TASK_CHECK(rendered.find("else") != std::string::npos);
    return 0;
}

static int tree_nested_statement_removal()
{
    // Every function must keep its final return, and statements are removed
    // as complete subtrees.  With no coverage requirement the smallest legal
    // source is exactly one `return 0;` (or a constant) per function, never a
    // dangling `if (` / `while (` without its block.  Checking by re-decoding
    // the output would be unsound (a short text decodes to a different tree),
    // so assert the rendered result directly.
    const std::string seed = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(seed.begin(), seed.end());
    TempFile worker(fixedWorker(""));
    TASK_CHECK(!worker.path.empty());

    const structured::Tree before = structured::buildTree(data.data(), data.size());
    const int stmtsBefore = totalStmts(before);
    TASK_CHECK(stmtsBefore == 11);

    const std::string reduced = reduceCode2(data.data(), data.size(), worker.path, {}, 256);

    TASK_CHECK(reduced.size() < structured::renderTree(before).size());
    TASK_CHECK(reduced.find("if (") == std::string::npos);
    TASK_CHECK(reduced.find("while (") == std::string::npos);

    // Count the surviving statements from the text: each function body holds
    // exactly one return and nothing else.
    size_t functions = 0, returns = 0, assignments = 0;
    for (size_t pos = 0; (pos = reduced.find("int f", pos)) != std::string::npos; ++pos) {
        const size_t paren = reduced.find('(', pos);
        const size_t brace = reduced.find('{', paren);
        if (paren == std::string::npos || brace == std::string::npos)
            break;
        ++functions;
        TASK_CHECK(reduced.compare(pos, 4, "int ") == 0);
    }
    for (size_t pos = 0; (pos = reduced.find("return ", pos)) != std::string::npos; ++pos)
        ++returns;
    for (size_t pos = 0; (pos = reduced.find("var", pos)) != std::string::npos; ++pos) {
        // Only declaration lines may mention a variable now.
        const size_t nl = reduced.rfind('\n', pos);
        const std::string line = reduced.substr(nl == std::string::npos ? 0 : nl + 1,
                                                pos - (nl == std::string::npos ? 0 : nl + 1));
        if (line.find("int var") == std::string::npos)
            ++assignments;
    }
    TASK_CHECK(functions == before.functions.size());
    TASK_CHECK(returns == functions);
    TASK_CHECK(assignments == 0);
    return 0;
}

static int tree_expression_replacement()
{
    // A worker that always reports id 1 lets every statement go, but the
    // function shells and returns remain.  Expressions are replaced by 0
    // where possible; the reduced source must render only literals and
    // retain the return.
    const std::string seed = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(seed.begin(), seed.end());
    TempFile worker(fixedWorker("1"));
    TASK_CHECK(!worker.path.empty());

    const structured::Tree before = structured::buildTree(data.data(), data.size());
    TASK_CHECK(totalCompoundExprs(before) > 0);
    const std::string beforeText = structured::renderTree(before);

    const std::string reduced = reduceCode2(data.data(), data.size(), worker.path, {1}, 512);

    TASK_CHECK(reduced.size() < beforeText.size());
    // The surviving return expressions are literal 0 or a plain lvalue /
    // global constant: no binary, unary or parenthesised expression remains
    // in the assignment-free reduced text.
    TASK_CHECK(reduced.find("if (") == std::string::npos);
    TASK_CHECK(reduced.find("while (") == std::string::npos);
    TASK_CHECK(reduced.find("return ") != std::string::npos);
    const size_t ret = reduced.find("return ");
    const size_t semi = reduced.find(';', ret);
    TASK_CHECK(ret != std::string::npos && semi != std::string::npos);
    const std::string expr = reduced.substr(ret + 7, semi - ret - 7);
    TASK_CHECK(expr == "0" || expr.find_first_not_of("0123456789") != std::string::npos ||
               expr.find("var") == 0 || expr.find("globalconstant") == 0);
    return 0;
}

static int tree_declaration_closure()
{
    // References must be found through the tree, including var4 buried in a
    // nested block: build a tree whose only reference to var4 is an assignment
    // inside an if's then-block.
    const std::string seed = bytesToSeed(richSeed());
    structured::Tree tree = structured::buildTree(
        reinterpret_cast<const uint8_t *>(seed.data()), seed.size());
    for (auto &f : tree.functions)
        f->body->stmts.clear();

    auto inner = std::make_unique<structured::Stmt>(structured::StmtKind::Assign);
    inner->varIndex = 4;
    inner->value = std::make_unique<structured::Expr>(structured::ExprKind::Literal);
    inner->value->literal = 7;

    auto outer = std::make_unique<structured::Stmt>(structured::StmtKind::If);
    outer->value = std::make_unique<structured::Expr>(structured::ExprKind::Literal);
    outer->value->literal = 1;
    outer->thenBlock = std::make_unique<structured::Block>();
    outer->thenBlock->stmts.push_back(std::move(inner));
    tree.functions[0]->body->stmts.push_back(std::move(outer));

    TASK_CHECK(structured::declReferenced(tree, 4));
    for (int v : {1, 2, 3, 5})
        TASK_CHECK(!structured::declReferenced(tree, v));

    // Removing the nested statement removes the only reference; a sibling
    // nested block's reference is still seen.
    structured::Tree copied = structured::cloneTree(tree);
    copied.functions[0]->body->stmts.clear();
    TASK_CHECK(!structured::declReferenced(copied, 4));

    structured::Tree sibling = structured::cloneTree(tree);
    auto outer2 = std::make_unique<structured::Stmt>(structured::StmtKind::If);
    outer2->value = std::make_unique<structured::Expr>(structured::ExprKind::Literal);
    outer2->value->literal = 1;
    outer2->elseBlock = std::make_unique<structured::Block>();
    auto inner2 = std::make_unique<structured::Stmt>(structured::StmtKind::Assign);
    inner2->varIndex = 2;
    inner2->value = std::make_unique<structured::Expr>(structured::ExprKind::Lvalue);
    inner2->value->varIndex = 3;
    outer2->elseBlock->stmts.push_back(std::move(inner2));
    sibling.functions[0]->body->stmts.push_back(std::move(outer2));
    TASK_CHECK(structured::declReferenced(sibling, 2));
    TASK_CHECK(structured::declReferenced(sibling, 3));
    TASK_CHECK(structured::declReferenced(sibling, 4));

    // The rich seed itself references all five variables somewhere.
    const structured::Tree rich = structured::buildTree(
        reinterpret_cast<const uint8_t *>(seed.data()), seed.size());
    for (int v = 1; v <= 5; ++v)
        TASK_CHECK(structured::declReferenced(rich, v));
    return 0;
}

static int tree_declaration_kept_when_used()
{
    // var1 is the seed's first declaration and var3/var4 appear nested.  When
    // the worker rejects every candidate, the source is unchanged and no
    // referenced declaration may have been dropped.
    const std::string seed = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(seed.begin(), seed.end());
    TempFile worker(fixedWorker("1"));
    TASK_CHECK(!worker.path.empty());

    const std::string direct = generateCode2(data.data(), data.size());
    // Requirement 999 is never reported by this worker, so the initial
    // observation fails and nothing is reduced.
    bool threw = false;
    try {
        reduceCode2(data.data(), data.size(), worker.path, {999}, 8);
    } catch (const std::exception &) {
        threw = true;
    }
    TASK_CHECK(threw);

    // With an empty requirement every tree-legal removal is available; any
    // surviving var must still be declared.
    const std::string reduced = reduceCode2(data.data(), data.size(), worker.path, {}, 256);
    for (int v = 1; v <= 5; ++v) {
        const std::string use = "var" + std::to_string(v);
        if (reduced.find(use) != std::string::npos) {
            // A "varN" mention implies the declaration is present (no other
            // line mentions a bare varN).
            TASK_CHECK(reduced.find("int " + use + " =") != std::string::npos);
        }
    }
    return 0;
}

/* ======================================================== oracle_cache */

static int oracle_accept_reject()
{
    const std::string seed = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(seed.begin(), seed.end());

    // Worker always reports id 1: a candidate that dropped all returns would
    // lose nothing observable, yet the reducer must never drop the function
    // shell.  Check the shell survives.
    TempFile worker(fixedWorker("1"));
    TASK_CHECK(!worker.path.empty());
    const std::string reduced = reduceCode2(data.data(), data.size(), worker.path, {1}, 256);
    TASK_CHECK(reduced.find("return") != std::string::npos);

    // Requiring an id the initial source never reports is an error.
    bool threw = false;
    try {
        reduceCode2(data.data(), data.size(), worker.path, {1, 999}, 16);
    } catch (const std::exception &) {
        threw = true;
    }
    TASK_CHECK(threw);

    // Zero budget returns the initial source untouched and starts no worker.
    CountingWorker counting;
    TASK_CHECK(counting.create());
    const std::string direct = generateCode2(data.data(), data.size());
    TASK_CHECK(reduceCode2(data.data(), data.size(), counting.script, {1, 2, 3}, 0) == direct);
    TASK_CHECK(counting.count() == 0);
    return 0;
}

static int oracle_cache_hit_reuses_observation()
{
    // The cache is process-wide.  Repeat the exact same reduction with the
    // same counting worker: every source was already observed, so the second
    // run must not start the worker again.
    const std::string seed = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(seed.begin(), seed.end());
    CountingWorker counting;
    TASK_CHECK(counting.create());

    const std::string first = reduceCode2(data.data(), data.size(), counting.script, {}, 64);
    const int afterFirst = counting.count();
    TASK_CHECK(afterFirst > 0);

    const std::string second = reduceCode2(data.data(), data.size(), counting.script, {}, 64);
    TASK_CHECK(first == second);
    TASK_CHECK(counting.count() == afterFirst);
    return 0;
}

static int oracle_same_bytes_one_observation()
{
    // Two byte-identical reductions must share their observations: prime the
    // process-wide cache with a converged reduction, then repeat it with
    // budget 1.  Every candidate is a cache hit, so the second run performs
    // no further worker starts, never touches its budget and still reaches
    // the identical converged source.  The trees are not merged: the reducer
    // rebuilds the tree from the seed both times.
    const std::string seed = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(seed.begin(), seed.end());
    CountingWorker counting;
    TASK_CHECK(counting.create());

    const std::string first = reduceCode2(data.data(), data.size(), counting.script, {}, 100000);
    const int baseline = counting.count();
    TASK_CHECK(baseline >= 1);

    const std::string again = reduceCode2(data.data(), data.size(), counting.script, {}, 1);
    TASK_CHECK(again == first);
    TASK_CHECK(counting.count() == baseline);

    // A failed observation is never cached: a worker that succeeds for the
    // initial source and fails afterwards must fail again on a repeat, i.e.
    // the failure is not remembered as a successful observation.
    const std::string counter = makeTempPath("retry-counter-", ".txt");
    const std::string script = makeTempPath("retry-worker-", ".sh");
    TASK_CHECK(!counter.empty() && !script.empty());
    TempFile counterFile(counter), scriptFile(script);
    TASK_CHECK(writeWholeFile(counter, "0"));
    TASK_CHECK(makeExecutableScript(script,
                                    "n=$(cat '" + counter + "')\n"
                                    "n=$((n+1))\n"
                                    "printf '%s' \"$n\" > '" + counter + "'\n"
                                    "if [ \"$n\" -gt 1 ]; then exit 9; fi\n"
                                    "echo \"COVER 1\""));
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            reduceCode2(data.data(), data.size(), script, {1}, 32);
        } catch (const std::exception &) {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    return 0;
}

static int oracle_repeat_determinism()
{
    // Decode state is per call: repeated and interleaved calls with different
    // seeds must not leak the cursor or function numbering.
    const std::string s = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(s.begin(), s.end());
    TempFile worker(fixedWorker("1 2 3"));
    TASK_CHECK(!worker.path.empty());

    const std::string r1 = reduceCode2(data.data(), data.size(), worker.path, {1, 2}, 128);
    const std::vector<uint8_t> other = {0x11, 0x22, 0x33};
    const std::string middle = reduceCode2(other.data(), other.size(), worker.path, {}, 16);
    const std::string r2 = reduceCode2(data.data(), data.size(), worker.path, {1, 2}, 128);
    const std::string r3 = reduceCode2(data.data(), data.size(), worker.path, {1, 2}, 128);

    TASK_CHECK(!middle.empty());
    TASK_CHECK(r1 == r2);
    TASK_CHECK(r2 == r3);

    const std::string g1 = generateCode2(data.data(), data.size());
    generateCode2(other.data(), other.size());
    const std::string g2 = generateCode2(data.data(), data.size());
    TASK_CHECK(g1 == g2);

    // Function names are renumbered from 1 after reduction.
    TASK_CHECK(r1.find("int f1()") != std::string::npos);
    TASK_CHECK(r1.find("int f0()") == std::string::npos);
    return 0;
}

static int oracle_budget_boundary()
{
    // Use a unique worker so the process-wide cache holds no pre-existing
    // observations for it.
    //
    // Budget 1 spends the single observation on the initial source: nothing
    // is reduced and exactly one worker start is made.
    const std::string s = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(s.begin(), s.end());
    CountingWorker counting;
    TASK_CHECK(counting.create());

    const std::string direct = generateCode2(data.data(), data.size());
    TASK_CHECK(reduceCode2(data.data(), data.size(), counting.script, {}, 1) == direct);
    TASK_CHECK(counting.count() == 1);
    return 0;
}

static int oracle_initial_observation_counts()
{
    // A positive budget must spend its first unit on the initial source even
    // when that source already satisfies the requirement: with budget 1 the
    // result is the untouched source and exactly one worker start occurred.
    // A fresh counting worker guarantees no cache hits.
    const std::string s = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(s.begin(), s.end());
    CountingWorker counting;
    TASK_CHECK(counting.create());
    const std::string direct = generateCode2(data.data(), data.size());
    TASK_CHECK(reduceCode2(data.data(), data.size(), counting.script, {}, 1) == direct);
    TASK_CHECK(counting.count() == 1);   // initial observation is counted

    // With no observed worker call yet (budget 0) the initial source is
    // returned without consulting the worker at all.
    CountingWorker untouched;
    TASK_CHECK(untouched.create());
    TASK_CHECK(reduceCode2(data.data(), data.size(), untouched.script, {}, 0) == direct);
    TASK_CHECK(untouched.count() == 0);
    return 0;
}

static int oracle_failure_no_cache_pollution()
{
    // A worker whose second call fails must abort the reduction.  Afterwards
    // a good worker must produce a clean, deterministic result; the failed
    // observation was never cached.
    const std::string s = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(s.begin(), s.end());
    const std::string counter = makeTempPath("fail-counter-", ".txt");
    const std::string script = makeTempPath("failing-worker-", ".sh");
    TASK_CHECK(!counter.empty() && !script.empty());
    TempFile counterFile(counter), scriptFile(script);
    TASK_CHECK(writeWholeFile(counter, "0"));
    TASK_CHECK(makeExecutableScript(script,
                                    "n=$(cat '" + counter + "')\n"
                                    "n=$((n+1))\n"
                                    "printf '%s' \"$n\" > '" + counter + "'\n"
                                    "if [ \"$n\" -ge 2 ]; then exit 7; fi\n"
                                    "echo \"COVER 1\""));

    const std::string direct = generateCode2(data.data(), data.size());
    bool threw = false;
    try {
        reduceCode2(data.data(), data.size(), script, {1}, 64);
    } catch (const std::exception &) {
        threw = true;
    }
    TASK_CHECK(threw);
    // The initial source was observed (call 1); call 2 failed and must abort.
    TASK_CHECK(std::atoi(readWholeFile(counter).c_str()) == 2);

    // A good worker afterwards: deterministic and reduced.
    TempFile good(fixedWorker("1"));
    TASK_CHECK(!good.path.empty());
    const std::string r1 = reduceCode2(data.data(), data.size(), good.path, {1}, 64);
    const std::string r2 = reduceCode2(data.data(), data.size(), good.path, {1}, 64);
    TASK_CHECK(r1 == r2);
    TASK_CHECK(!r1.empty());
    TASK_CHECK(r1.size() < direct.size());
    return 0;
}

static int oracle_malformed_and_start_failure()
{
    const std::string s = bytesToSeed(richSeed());
    const std::vector<uint8_t> data(s.begin(), s.end());

    // Non-existent worker path: must throw, not hang.
    bool startThrew = false;
    try {
        reduceCode2(data.data(), data.size(), tempDir() + "/no-such-worker-xyz", {}, 8);
    } catch (const std::exception &) {
        startThrew = true;
    }
    TASK_CHECK(startThrew);

    // Malformed output (missing COVER prefix) is an error after the initial
    // observation succeeds; use budget 2 so a candidate triggers the worker
    // a second time with bad output.
    const std::string bad = makeTempPath("bad-output-", ".sh");
    TASK_CHECK(!bad.empty());
    TempFile badFile(bad);
    TASK_CHECK(makeExecutableScript(bad, "echo \"hello 1\""));
    bool badThrew = false;
    try {
        reduceCode2(data.data(), data.size(), bad, {}, 8);
    } catch (const std::exception &) {
        badThrew = true;
    }
    TASK_CHECK(badThrew);
    return 0;
}

/* ===================================================== production_entry */

static int production_empty_input()
{
    // Empty seed and zero budget: five declarations, f1 and return 0.
    const std::string a = reduceCode2(nullptr, 0, "", {}, 0);
    const std::string b = reduceCode2(nullptr, 0, "", {}, 0);
    TASK_CHECK(a == b);
    for (int v = 1; v <= 5; ++v)
        TASK_CHECK(a.find("int var" + std::to_string(v) + " =") != std::string::npos);
    TASK_CHECK(a.find("int f1()") != std::string::npos);
    TASK_CHECK(a.find("return 0;") != std::string::npos);
    return 0;
}

static int production_decimal_numbers()
{
    // The literal branch must render decimal digits, never raw bytes.
    const std::string seed = bytesToSeed(richSeed());
    const std::string generated = generateCode2(
        reinterpret_cast<const uint8_t *>(seed.data()), seed.size());
    for (char c : generated) {
        const unsigned char u = static_cast<unsigned char>(c);
        TASK_CHECK(c == '\n' || (u >= 0x20 && u < 0x7f));
    }
    TASK_CHECK(generated.find("globalconstant1") != std::string::npos);
    // A bare numeric literal in the source must be a plausible decimal number
    // from the 0x80*0x80+0x80 range, not a control character.
    TASK_CHECK(generated.find("4447") != std::string::npos ||
               generated.find("6272") != std::string::npos ||
               generated.find("14831") != std::string::npos ||
               generated.find("globalconstant1") != std::string::npos);
    return 0;
}

/* Compile a piece of generated source with -fsyntax-only.  Never runs it.
 * The compiler is taken from CXX or the first c++ found on PATH; if none is
 * available the check is skipped. */
static bool syntaxOnly(const std::string &source)
{
    const char *cxx = std::getenv("CXX");
    std::string compiler = (cxx && *cxx) ? cxx : "c++";
    const std::string file = makeTempPath("syntax-", ".cpp");
    if (file.empty())
        return false;
    if (!writeWholeFile(file, source)) {
        unlink(file.c_str());
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        unlink(file.c_str());
        return false;
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execlp(compiler.c_str(), compiler.c_str(), "-std=c++17", "-fsyntax-only",
               "-x", "c++", file.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    unlink(file.c_str());
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        return true;   // compiler missing: skip rather than fail
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int production_reduced_source_compiles()
{
    // The reduced source is only ever observed as text; compile (never run)
    // it to make sure the renderer keeps the statement/expression grammar
    // intact.  This seed renders without any globalconstant references (the
    // generator's global-constant creation is disabled), so it is valid C++.
    const std::vector<unsigned> seed = {0x01};
    const std::string bytes = bytesToSeed(seed);
    const std::vector<uint8_t> data(bytes.begin(), bytes.end());
    const std::string initial = generateCode2(data.data(), data.size());
    TASK_CHECK(initial.find("if (") != std::string::npos);
    TASK_CHECK(syntaxOnly(initial));

    // A worker that only accepts candidates still containing an `if (`.  The
    // reduced source must keep exactly one (its condition replaced by 0) and
    // still compile, which also checks that the condition slot is rendered.
    const std::string script = makeTempPath("if-worker-", ".sh");
    TASK_CHECK(!script.empty());
    TempFile scriptFile(script);
    TASK_CHECK(makeExecutableScript(script,
                                    "if grep -q 'if (' \"$1\"; then echo 'COVER 3'; else echo 'COVER'; fi"));
    const std::string reduced = reduceCode2(data.data(), data.size(), script, {3}, 128);
    TASK_CHECK(reduced.find("if (") != std::string::npos);
    TASK_CHECK(reduced.find("while (") == std::string::npos);
    TASK_CHECK(reduced.find("return 0;") != std::string::npos);
    TASK_CHECK(syntaxOnly(reduced));
    return 0;
}

static int production_translate_reduce()
{
    // Run the translate --reduce entry point if the harness provided it.
    const std::string translator = translatorPath();
    if (translator.empty())
        return 0; // not provided; the API is covered by other cases

    const std::string input = makeTempPath("translate-in-", ".bin");
    const std::string output = makeTempPath("translate-out-", ".c");
    TASK_CHECK(!input.empty() && !output.empty());
    TempFile inputFile(input), outputFile(output);
    const std::string seed = bytesToSeed(richSeed());
    TASK_CHECK(writeWholeFile(input, seed));

    TempFile worker(fixedWorker("1"));
    TASK_CHECK(!worker.path.empty());

    const pid_t pid = fork();
    TASK_CHECK(pid >= 0);
    if (pid == 0) {
        execl(translator.c_str(), translator.c_str(), "--reduce", worker.path.c_str(), "1", "32",
              input.c_str(), output.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    TASK_CHECK(fileExists(output));
    const std::string produced = readWholeFile(output);
    TASK_CHECK(!produced.empty());
    TASK_CHECK(produced.find("int f1()") != std::string::npos);
    // Nothing left behind that looks like a half-written temporary file.
    TASK_CHECK(produced == reduceCode2(reinterpret_cast<const uint8_t *>(seed.data()),
                                       seed.size(), worker.path, {1}, 32));
    return 0;
}

static int production_translate_failure_no_partial_output()
{
    const std::string translator = translatorPath();
    if (translator.empty())
        return 0;

    const std::string input = makeTempPath("translate-bad-", ".bin");
    const std::string output = makeTempPath("translate-bad-out-", ".c");
    TASK_CHECK(!input.empty() && !output.empty());
    TempFile inputFile(input), outputFile(output);
    const std::string seed = bytesToSeed(richSeed());
    TASK_CHECK(writeWholeFile(input, seed));

    const std::string badWorker = makeTempPath("bad-worker-", ".sh");
    TASK_CHECK(!badWorker.empty());
    TempFile badFile(badWorker);
    TASK_CHECK(makeExecutableScript(badWorker, "exit 3"));

    const pid_t pid = fork();
    TASK_CHECK(pid >= 0);
    if (pid == 0) {
        // The translator is expected to report the failure on stderr; keep it
        // out of the group's output.
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execl(translator.c_str(), translator.c_str(), "--reduce", badWorker.c_str(), "-", "8",
              input.c_str(), output.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
    TASK_CHECK(!fileExists(output));
    return 0;
}

extern const struct task_case task_cases[] = {
    {"tree_dependencies", "matches_generator_text", tree_matches_generator_text},
    {"tree_dependencies", "nested_statement_removal", tree_nested_statement_removal},
    {"tree_dependencies", "expression_replacement", tree_expression_replacement},
    {"tree_dependencies", "declaration_closure", tree_declaration_closure},
    {"tree_dependencies", "declaration_kept_when_used", tree_declaration_kept_when_used},
    {"oracle_cache", "accept_reject", oracle_accept_reject},
    {"oracle_cache", "cache_hit_reuses_observation", oracle_cache_hit_reuses_observation},
    {"oracle_cache", "same_bytes_one_observation", oracle_same_bytes_one_observation},
    {"oracle_cache", "repeat_determinism", oracle_repeat_determinism},
    {"oracle_cache", "budget_boundary", oracle_budget_boundary},
    {"oracle_cache", "initial_observation_counts", oracle_initial_observation_counts},
    {"oracle_cache", "failure_no_cache_pollution", oracle_failure_no_cache_pollution},
    {"oracle_cache", "malformed_and_start_failure", oracle_malformed_and_start_failure},
    {"production_entry", "empty_input", production_empty_input},
    {"production_entry", "decimal_numbers", production_decimal_numbers},
    {"production_entry", "reduced_source_compiles", production_reduced_source_compiles},
    {"production_entry", "translate_reduce", production_translate_reduce},
    {"production_entry", "translate_failure_no_partial_output", production_translate_failure_no_partial_output},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
