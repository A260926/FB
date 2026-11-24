/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2024 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "structured.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace structured {

namespace {

std::unique_ptr<Expr> cloneExpr(const Expr &e)
{
    auto c = std::make_unique<Expr>(e.kind);
    c->varIndex = e.varIndex;
    c->literal = e.literal;
    c->globalIndex = e.globalIndex;
    c->op = e.op;
    c->unaryOp = e.unaryOp;
    c->compOp = e.compOp;
    c->wrapParen = e.wrapParen;
    if (e.lhs)
        c->lhs = cloneExpr(*e.lhs);
    if (e.rhs)
        c->rhs = cloneExpr(*e.rhs);
    return c;
}

std::unique_ptr<Stmt> cloneStmt(const Stmt &s);
std::unique_ptr<Block> cloneBlock(const Block &b);

std::unique_ptr<Stmt> cloneStmt(const Stmt &s)
{
    auto c = std::make_unique<Stmt>(s.kind);
    c->varIndex = s.varIndex;
    if (s.value)
        c->value = cloneExpr(*s.value);
    if (s.thenBlock)
        c->thenBlock = cloneBlock(*s.thenBlock);
    if (s.elseBlock)
        c->elseBlock = cloneBlock(*s.elseBlock);
    return c;
}

std::unique_ptr<Block> cloneBlock(const Block &b)
{
    auto c = std::make_unique<Block>();
    for (const auto &s : b.stmts)
        c->stmts.push_back(cloneStmt(*s));
    return c;
}

} // namespace

Tree cloneTree(const Tree &tree)
{
    Tree result;
    result.decls = tree.decls;
    for (const auto &f : tree.functions) {
        auto nf = std::make_unique<Function>();
        nf->body = cloneBlock(*f->body);
        result.functions.push_back(std::move(nf));
    }
    return result;
}

namespace {

bool isCompound(ExprKind k)
{
    switch (k) {
    case ExprKind::Binary:
    case ExprKind::Assign:
    case ExprKind::Unary:
    case ExprKind::Paren:
    case ExprKind::Compare:
        return true;
    case ExprKind::Lvalue:
    case ExprKind::GlobalConstant:
    case ExprKind::Literal:
        break;
    }
    return false;
}

/* One navigation step into a block: index of the statement to enter, and
 * which child block to continue into (0 = then, 1 = else, -1 = stop here). */
struct Step {
    size_t index;
    int block;
};

struct StmtPath {
    size_t fn = 0;
    std::vector<Step> steps;
};

bool eraseStmt(Block &block, const std::vector<Step> &steps, size_t depth)
{
    const size_t index = steps[depth].index;
    if (index >= block.stmts.size())
        return false;
    if (depth + 1 == steps.size() && steps[depth].block == -1) {
        block.stmts.erase(block.stmts.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    const int which = steps[depth].block;
    if (which != 0 && which != 1)
        return false;
    Stmt &s = *block.stmts[index];
    Block *child = (which == 0) ? s.thenBlock.get() : s.elseBlock.get();
    if (!child)
        return false;
    return eraseStmt(*child, steps, depth + 1);
}

Stmt *findStmt(Tree &tree, const StmtPath &path)
{
    if (path.fn >= tree.functions.size() || path.steps.empty())
        return nullptr;
    Block *block = tree.functions[path.fn]->body.get();
    for (size_t i = 0; i < path.steps.size(); ++i) {
        const Step &step = path.steps[i];
        if (step.index >= block->stmts.size())
            return nullptr;
        Stmt &s = *block->stmts[step.index];
        if (i + 1 == path.steps.size())
            return &s;
        Block *child = (step.block == 0) ? s.thenBlock.get()
                      : (step.block == 1) ? s.elseBlock.get() : nullptr;
        if (!child)
            return nullptr;
        block = child;
    }
    return nullptr;
}

std::unique_ptr<Expr> *findExprSlot(Tree &tree, const StmtPath &path, const std::vector<int> &children)
{
    Stmt *s = findStmt(tree, path);
    if (!s || !s->value)
        return nullptr;
    std::unique_ptr<Expr> *slot = &s->value;
    for (int c : children) {
        Expr *e = slot->get();
        if (!e)
            return nullptr;
        slot = (c == 0) ? &e->lhs : &e->rhs;
        if (!slot->get())
            return nullptr;
    }
    return slot;
}

/* ------------------------------------------------------------------ Oracle */

struct BudgetExhausted {};

/* Observations are cached for the life of the process, keyed by the complete
 * normalized source and the worker that produced it.  A cache hit never
 * consumes budget; unfinished (failed) observations are never inserted. */
std::map<std::pair<std::string, std::string>, std::set<unsigned>> &observationCache()
{
    static std::map<std::pair<std::string, std::string>, std::set<unsigned>> cache;
    return cache;
}

std::mutex &observationCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

class Oracle {
public:
    Oracle(std::string worker, std::set<unsigned> required, size_t budget)
        : m_worker(std::move(worker)), m_required(std::move(required)), m_budget(budget) {}

    std::set<unsigned> observe(const std::string &source)
    {
        const std::pair<std::string, std::string> key(m_worker, source);
        {
            std::lock_guard<std::mutex> lock(observationCacheMutex());
            const auto cached = observationCache().find(key);
            if (cached != observationCache().end())
                return cached->second;   // cache hits never consume budget
        }

        if (m_spent >= m_budget)
            throw BudgetExhausted();

        std::set<unsigned> ids = runWorker(source);
        ++m_spent;
        // Successful observations are cached by the complete normalized
        // source, including observations that miss a required id.
        {
            std::lock_guard<std::mutex> lock(observationCacheMutex());
            observationCache().emplace(key, ids);
        }
        return ids;
    }

    bool satisfies(const std::set<unsigned> &ids) const
    {
        return std::includes(ids.begin(), ids.end(), m_required.begin(), m_required.end());
    }

private:
    static std::set<unsigned> parseOutput(const std::string &out);
    std::set<unsigned> runWorker(const std::string &source);

    std::string m_worker;
    std::set<unsigned> m_required;
    size_t m_budget;
    size_t m_spent = 0;
};

class TempSource {
public:
    explicit TempSource(const std::string &content)
    {
        const char *dirEnv = std::getenv("TMPDIR");
        std::string dir = (dirEnv && *dirEnv) ? dirEnv : "/tmp";
        if (dir.back() != '/')
            dir += '/';
        std::string pattern = dir + "structured-reduce-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');

        const int fd = mkstemp(name.data());
        if (fd < 0)
            throw std::runtime_error(std::string("reduceCode2: cannot create temporary source file: ") + std::strerror(errno));
        m_path.assign(name.data());

        const char *p = content.data();
        size_t left = content.size();
        while (left) {
            const ssize_t n = write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                const std::string err = std::strerror(errno);
                close(fd);
                unlink(m_path.c_str());
                throw std::runtime_error("reduceCode2: cannot write temporary source file: " + err);
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        if (close(fd) != 0) {
            const std::string err = std::strerror(errno);
            unlink(m_path.c_str());
            throw std::runtime_error("reduceCode2: cannot close temporary source file: " + err);
        }
    }

    ~TempSource()
    {
        if (!m_path.empty())
            unlink(m_path.c_str());
    }

    TempSource(const TempSource &) = delete;
    TempSource &operator=(const TempSource &) = delete;

    const std::string &path() const { return m_path; }

private:
    std::string m_path;
};

std::set<unsigned> Oracle::parseOutput(const std::string &raw)
{
    std::string line = raw;
    if (!line.empty() && line.back() == '\n')
        line.pop_back();
    if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos)
        throw std::runtime_error("reduceCode2: worker produced more than one line");

    std::set<unsigned> ids;
    size_t pos = 0;
    bool first = true;
    while (pos <= line.size()) {
        const size_t space = line.find(' ', pos);
        const std::string token = line.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
        if (first) {
            if (token != "COVER")
                throw std::runtime_error("reduceCode2: worker output does not start with COVER");
            first = false;
        } else {
            if (token.empty())
                throw std::runtime_error("reduceCode2: malformed coverage id list");
            unsigned value = 0;
            for (char c : token) {
                if (c < '0' || c > '9')
                    throw std::runtime_error("reduceCode2: malformed coverage id");
                const unsigned digit = static_cast<unsigned>(c - '0');
                if (value > (0xFFFFFFFFu - digit) / 10)
                    throw std::runtime_error("reduceCode2: coverage id out of range");
                value = value * 10 + digit;
            }
            ids.insert(value);
        }
        if (space == std::string::npos)
            break;
        pos = space + 1;
    }
    if (first)
        throw std::runtime_error("reduceCode2: empty worker output");
    return ids;
}

std::set<unsigned> Oracle::runWorker(const std::string &source)
{
    TempSource temp(source);

    int outPipe[2];
    int errPipe[2];
    if (pipe(outPipe) != 0)
        throw std::runtime_error(std::string("reduceCode2: pipe failed: ") + std::strerror(errno));
    if (pipe(errPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        throw std::runtime_error(std::string("reduceCode2: pipe failed: ") + std::strerror(errno));
    }
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        throw std::runtime_error(std::string("reduceCode2: fork failed: ") + std::strerror(errno));
    }

    if (pid == 0) {
        setpgid(0, 0);
        close(outPipe[0]);
        close(errPipe[0]);
        if (dup2(outPipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (outPipe[1] != STDOUT_FILENO)
            close(outPipe[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull != STDERR_FILENO)
                close(devnull);
        }
        const char *args[] = {m_worker.c_str(), temp.path().c_str(), nullptr};
        execv(m_worker.c_str(), const_cast<char *const *>(args));
        const int error = errno;
        const ssize_t ignored = write(errPipe[1], &error, sizeof(error));
        (void)ignored;
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string output;
    bool timedOut = false;
    bool tooLarge = false;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timedOut = true;
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd;
        pfd.fd = outPipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int ready = poll(&pfd, 1, static_cast<int>(remaining) + 1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0) {
            timedOut = true;
            break;
        }
        char buffer[4096];
        const ssize_t n = read(outPipe[0], buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;   // EOF
        output.append(buffer, static_cast<size_t>(n));
        if (output.size() > (1u << 20)) {
            tooLarge = true;   // a single COVER line can never be this long
            break;
        }
    }

    close(outPipe[0]);

    int status = 0;
    bool reaped = false;
    if (!timedOut) {
        for (;;) {
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reaped = true;
                break;
            }
            if (r < 0 && errno != EINTR) {
                reaped = true;
                status = -1;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, nullptr);
        }
    }

    if (timedOut) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int ignored = 0;
        while (waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {
        }
        close(errPipe[0]);
        throw std::runtime_error("reduceCode2: worker exceeded the 1 second limit");
    }

    // Drain until EOF so the exec-error report (if any) is fully read.  The
    // child closed the write end on a successful exec, so this does not block.
    int execError = 0;
    size_t errBytes = 0;
    for (;;) {
        char buffer[sizeof(execError)];
        const ssize_t n = read(errPipe[0], buffer, sizeof(buffer) - errBytes);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        std::memcpy(reinterpret_cast<char *>(&execError) + errBytes, buffer, static_cast<size_t>(n));
        errBytes += static_cast<size_t>(n);
        if (errBytes == sizeof(execError))
            break;
    }
    close(errPipe[0]);

    // The child's exit status is authoritative for successful exec: if it
    // ran, it owns any output we read.  A non-zero status is a worker failure
    // even when it emitted nothing or was killed by the buffer cap.
    const bool execFailed = !WIFEXITED(status) || WEXITSTATUS(status) == 127;
    if (!reaped && !execFailed)
        throw std::runtime_error("reduceCode2: lost track of worker process");

    if (execFailed || errBytes == sizeof(execError)) {
        if (!reaped) {
            int ignored = 0;
            while (waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {
            }
        }
        const int err = (errBytes == sizeof(execError)) ? execError : ENOENT;
        throw std::runtime_error("reduceCode2: cannot start worker '" + m_worker + "': " + std::strerror(err));
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("reduceCode2: worker exited unsuccessfully");

    if (tooLarge)
        throw std::runtime_error("reduceCode2: worker output is too large");

    return parseOutput(output);
}

/* ----------------------------------------------------------------- Reducer */

class Reducer {
public:
    Reducer(Tree &tree, std::string &current, Oracle &oracle)
        : m_tree(tree), m_current(current), m_oracle(oracle) {}

    // Returns true when a candidate was accepted (the caller restarts).
    bool tryCandidate(Tree candidate)
    {
        std::string rendered = renderTree(candidate);
        if (rendered.size() >= m_current.size())
            return false;   // cannot satisfy the strict size decrease
        std::set<unsigned> ids = m_oracle.observe(rendered);
        if (!m_oracle.satisfies(ids))
            return false;
        m_tree = std::move(candidate);
        m_current = std::move(rendered);
        return true;
    }

    bool deleteStatementPass()
    {
        for (size_t fn = 0; fn < m_tree.functions.size(); ++fn) {
            StmtPath path;
            path.fn = fn;
            if (deleteInBlock(*m_tree.functions[fn]->body, path, true))
                return true;
        }
        return false;
    }

    bool replaceExpressionPass()
    {
        for (size_t fn = 0; fn < m_tree.functions.size(); ++fn) {
            StmtPath path;
            path.fn = fn;
            if (replaceInBlock(*m_tree.functions[fn]->body, path))
                return true;
        }
        return false;
    }

    bool deleteDeclPass()
    {
        for (size_t i = 0; i < m_tree.decls.size(); ++i) {
            if (!m_tree.decls[i].present)
                continue;
            if (declReferenced(m_tree, m_tree.decls[i].varIndex))
                continue;
            Tree candidate = cloneTree(m_tree);
            candidate.decls[i].present = false;
            if (tryCandidate(std::move(candidate)))
                return true;
        }
        return false;
    }

    const std::string &current() const { return m_current; }

private:
    bool deleteInBlock(Block &block, StmtPath &path, bool functionBody)
    {
        for (size_t i = 0; i < block.stmts.size(); ++i) {
            Stmt &s = *block.stmts[i];
            const bool trailingReturn = functionBody && i + 1 == block.stmts.size() && s.kind == StmtKind::Return;

            path.steps.push_back(Step{i, -1});
            if (!trailingReturn) {
                Tree candidate = cloneTree(m_tree);
                if (eraseStmt(*candidate.functions[path.fn]->body, path.steps, 0) &&
                    tryCandidate(std::move(candidate)))
                    return true;
            }
            if (s.thenBlock) {
                path.steps.back().block = 0;
                if (deleteInBlock(*s.thenBlock, path, false))
                    return true;
            }
            if (s.elseBlock) {
                path.steps.back().block = 1;
                if (deleteInBlock(*s.elseBlock, path, false))
                    return true;
            }
            path.steps.back().block = -1;
            path.steps.pop_back();
        }
        return false;
    }

    bool replaceInBlock(Block &block, StmtPath &path)
    {
        for (size_t i = 0; i < block.stmts.size(); ++i) {
            Stmt &s = *block.stmts[i];
            path.steps.push_back(Step{i, -1});
            if (s.value) {
                std::vector<int> children;
                if (replaceInExpr(*s.value, path, children))
                    return true;
            }
            if (s.thenBlock) {
                path.steps.back().block = 0;
                if (replaceInBlock(*s.thenBlock, path))
                    return true;
            }
            if (s.elseBlock) {
                path.steps.back().block = 1;
                if (replaceInBlock(*s.elseBlock, path))
                    return true;
            }
            path.steps.back().block = -1;
            path.steps.pop_back();
        }
        return false;
    }

    bool replaceInExpr(const Expr &expr, const StmtPath &path, std::vector<int> &children)
    {
        if (isCompound(expr.kind)) {
            Tree candidate = cloneTree(m_tree);
            std::unique_ptr<Expr> *slot = findExprSlot(candidate, path, children);
            if (slot) {
                auto zero = std::make_unique<Expr>(ExprKind::Literal);
                zero->literal = 0;
                *slot = std::move(zero);
                if (tryCandidate(std::move(candidate)))
                    return true;
            }
        }
        if (expr.lhs) {
            children.push_back(0);
            if (replaceInExpr(*expr.lhs, path, children))
                return true;
            children.pop_back();
        }
        if (expr.rhs) {
            children.push_back(1);
            if (replaceInExpr(*expr.rhs, path, children))
                return true;
            children.pop_back();
        }
        return false;
    }

    Tree &m_tree;
    std::string &m_current;
    Oracle &m_oracle;
};

} // namespace

std::string reduceCode2Impl(const uint8_t *data,
                            size_t size,
                            const std::string &worker,
                            const std::vector<unsigned> &required,
                            size_t budget)
{
    Tree tree = buildTree(data, size);
    std::string current = renderTree(tree);

    if (budget == 0)
        return current;   // no worker is started

    std::set<unsigned> requiredIds(required.begin(), required.end());
    Oracle oracle(worker, requiredIds, budget);

    // The initial observation consumes budget too.
    const std::set<unsigned> initial = oracle.observe(current);
    if (!oracle.satisfies(initial))
        throw std::runtime_error("reduceCode2: initial source does not provide all required coverage");

    Reducer reducer(tree, current, oracle);
    try {
        for (;;) {
            if (reducer.deleteStatementPass())
                continue;
            if (reducer.replaceExpressionPass())
                continue;
            if (reducer.deleteDeclPass())
                continue;
            break;
        }
    } catch (const BudgetExhausted &) {
        // Normal stop: return what has been accepted so far.
    }

    return reducer.current();
}

} // namespace structured

// Public global entry point.  The implementation lives in namespace
// structured; this wrapper is what the library, translate and the tests bind
// to regardless of whether the header was included normally or with
// -include.
std::string reduceCode2(const uint8_t *data,
                        size_t size,
                        const std::string &worker,
                        const std::vector<unsigned> &required,
                        size_t budget)
{
    return structured::reduceCode2Impl(data, size, worker, required, budget);
}
