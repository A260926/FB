# Security Policy

## Supported Versions

racedetector is production-ready for development and testing. We provide security updates for the following versions:

| Version | Supported          | Notes |
| ------- | ------------------ | ----- |
| 0.3.x   | :white_check_mark: | Current stable (Go 1.24+ required) |
| 0.2.x   | :white_check_mark: | Performance optimizations |
| 0.1.x   | :x:                | Superseded |
| < 0.1.0 | :x:                | Development only |

Future stable releases (v1.0+) will follow semantic versioning with LTS support after Runtime Integration phase.

## Reporting a Vulnerability

We take security seriously. If you discover a security vulnerability in racedetector, please report it responsibly.

### How to Report

**DO NOT** open a public GitHub issue for security vulnerabilities.

Instead, please report security issues by:

1. **Private Security Advisory** (preferred):
   https://github.com/kolkov/racedetector/security/advisories/new

2. **Email** to maintainers:
   Create a private GitHub issue or contact via discussions

### What to Include

Please include the following information in your report:

- **Description** of the vulnerability
- **Steps to reproduce** the issue (include malicious Go source file if applicable)
- **Affected versions** (which versions are impacted)
- **Potential impact** (code execution, information disclosure, DoS, etc.)
- **Suggested fix** (if you have one)
- **Your contact information** (for follow-up questions)

### Response Timeline

- **Initial Response**: Within 48-72 hours
- **Triage & Assessment**: Within 1 week
- **Fix & Disclosure**: Coordinated with reporter

We aim to:
1. Acknowledge receipt within 72 hours
2. Provide an initial assessment within 1 week
3. Work with you on a coordinated disclosure timeline
4. Credit you in the security advisory (unless you prefer to remain anonymous)

## Security Considerations for Race Detection

racedetector instruments Go source code and tracks memory accesses at runtime. This introduces unique security considerations.

### 1. AST Parsing Vulnerabilities

**Risk**: Maliciously crafted Go source files could exploit AST parsing.

**Attack Vectors**:
- Deeply nested AST structures (stack overflow)
- Extremely large source files (memory exhaustion)
- Crafted syntax that triggers parser bugs
- Unicode exploitation in identifiers

**Mitigation in Library**:
- ✅ Using official `go/ast` package (battle-tested)
- ✅ Parsing performed by Go standard library
- ✅ Resource limits on file sizes
- ✅ Validation of AST structure before instrumentation

**User Recommendations**:
```go
// ❌ BAD - Don't instrument untrusted source code without validation
racedetector build untrusted_malicious.go

// ✅ GOOD - Validate source files first
// Only instrument trusted code from your repository
racedetector build ./internal/myapp/...
```

### 2. Code Injection Risks

**Risk**: Instrumentation modifies source code by injecting imports and function calls. Malicious source files could exploit this.

**Current Mitigation**:
- ✅ Read-only AST parsing (original files never modified)
- ✅ Instrumentation happens in temporary workspace
- ✅ Generated code uses safe imports (race package, unsafe)
- ✅ No user-controlled code injection points
- ✅ All injected code is deterministic and validated

**Security Boundaries**:
```
Untrusted Input: Go source files
Trusted Output: Instrumented Go code
Validation: AST parsing + structure validation
Isolation: Temporary workspace, no original file modification
```

### 3. Race Detector Runtime Security

**Risk**: The race detector runtime tracks all memory accesses. Vulnerabilities could leak sensitive data.

**Attack Vectors**:
- Information disclosure via race reports (memory addresses, values)
- Memory exhaustion via unlimited shadow memory growth
- CPU exhaustion via expensive race detection on hot paths
- Denial of service via crafted concurrent workloads

**Mitigation**:
- ✅ Race reports show addresses, NOT values (no data leaks)
- ✅ Shadow memory uses sync.Map with GC-friendly design
- ✅ Performance overhead monitored (15-22% acceptable)
- ✅ No network communication (all data stays local)
- ✅ No logging to external services

**Security Features**:
- Race reports NEVER expose actual data values
- Stack traces show function names, not sensitive strings
- All race detection happens in-process (no IPC)
- No persistence of race data (runtime-only)

### 4. Temporary File Security

**Risk**: Build command creates temporary workspaces with instrumented code.

**Attack Vectors**:
- Path traversal in temp directory creation
- Race condition in temp file cleanup (TOCTOU)
- Information disclosure via temp files
- Symlink attacks on cleanup

**Mitigation**:
- ✅ Uses `os.MkdirTemp()` with secure random names
- ✅ Cleanup with `defer` pattern (exception-safe)
- ✅ Restrictive permissions on temp directories (0700)
- ✅ No predictable temp file names
- ✅ Cleanup even on panics

**Safe Temp File Usage**:
```go
// Internal implementation uses secure patterns
workspace := os.MkdirTemp("", "racedetector-*")  // Secure random
defer os.RemoveAll(workspace)                    // Always cleaned up
```

### 5. Dependency Security

racedetector has minimal dependencies:

**Production Dependencies**:
- `golang.org/x/mod v0.30.0` - Official Go module for go.mod parsing (required for replace directive handling)

**Development Dependencies**:
- `github.com/stretchr/testify` - Testing only
- Go toolchain (1.24+)

**Security Benefits**:
- ✅ Only one external dependency (golang.org/x/mod - official Go project)
- ✅ No C dependencies (Pure Go, no CGO)
- ✅ Minimal attack surface
- ✅ golang.org/x/* packages have same security standards as Go itself

**Monitoring**:
- ✅ Dependabot enabled (when repository goes public)
- ✅ Weekly dependency audit
- ✅ Linter with security checks (golangci-lint)

## Security Best Practices for Users

### 1. Only Instrument Trusted Code

**racedetector should ONLY be used on code you trust:**

```bash
# ✅ GOOD - Your own codebase
racedetector build ./cmd/myapp

# ✅ GOOD - Trusted dependencies
racedetector build ./internal/...

# ❌ BAD - Untrusted third-party code
racedetector build github.com/untrusted/malicious-package
```

**Why?** Instrumentation requires parsing and running code. Only use on code you would normally compile and run.

### 2. Validate Source Files

If you must instrument external code, validate first:

```bash
# Check file sizes
find . -name "*.go" -size +10M  # Flag suspiciously large files

# Check for suspicious patterns
grep -r "//go:linkname" .        # Unsafe compiler directives
grep -r "syscall\\.Syscall" .    # Direct system calls
```

### 3. Resource Limits in CI/CD

Set limits when using in automated environments:

```yaml
# .github/workflows/race-check.yml
- name: Race Detection
  run: |
    timeout 10m racedetector build ./...  # 10-minute timeout
  env:
    GOMAXPROCS: 4  # Limit CPU usage
```

### 4. Error Handling

Always check for instrumentation failures:

```bash
# ❌ BAD - Ignoring errors
racedetector build main.go || true

# ✅ GOOD - Fail on errors
racedetector build main.go
if [ $? -ne 0 ]; then
    echo "Instrumentation failed - potentially malicious code"
    exit 1
fi
```

### 5. Isolated Environments

Run race detection in isolated environments:

```bash
# Use containers for untrusted code
docker run --rm -v $(pwd):/app golang:1.24 \
    bash -c "cd /app && racedetector build ./..."
```

## Known Security Considerations

### 1. AST Manipulation Risks

**Status**: Mitigated via read-only parsing and isolated workspace.

**Risk Level**: Low

**Description**: Instrumentation modifies AST before compilation. Malicious source could exploit parser or code generator.

**Mitigation**:
- Uses official Go AST parser (standard library)
- Original files never modified (read-only)
- Generated code in isolated temp workspace
- No user-controlled injection points

### 2. Information Disclosure via Race Reports

**Status**: Mitigated - reports show addresses, not values.

**Risk Level**: Low

**Description**: Race reports could leak sensitive data from memory.

**Mitigation**:
- Reports show memory addresses (e.g., 0x00c0000180a0)
- Reports show goroutine IDs and stack traces
- **NEVER** shows actual memory values
- No network transmission of race data

**Example Safe Report**:
```
WARNING: DATA RACE
Write at 0x00c0000180a0 by goroutine 7:
  main.increment()
      main.go:25 +0x3b

Previous read at 0x00c0000180a0 by goroutine 6:
  main.increment()
      main.go:23 +0x2a
```

### 3. Performance DoS

**Status**: Acceptable overhead (15-22%).

**Risk Level**: Low

**Description**: Race detection adds runtime overhead. Malicious workloads could amplify this.

**Mitigation**:
- Overhead measured at 15-22% (acceptable)
- Hot path optimization with `//go:nosplit`
- Zero allocations on critical paths
- Users control when race detection runs (dev/test only)

**Recommendation**: Don't use in production - dev/test only!

### 4. Temporary Workspace Security

**Status**: Mitigated via secure temp file APIs.

**Risk Level**: Very Low

**Description**: Temp directories could be exploited (TOCTOU, path traversal).

**Mitigation**:
- Secure random temp names (`os.MkdirTemp`)
- Restrictive permissions (0700)
- Exception-safe cleanup (`defer`)
- No predictable paths

## Security Testing

### Current Testing

- ✅ 670+ tests with edge cases (100% pass rate)
- ✅ Integration tests with real race conditions
- ✅ Dogfooding (tool instruments itself)
- ✅ Linting with golangci-lint (34+ linters)
- ✅ Zero CGO (no memory safety issues from C)
- ✅ 86.3% test coverage

### Planned for v1.0

- 🔄 Fuzzing with go-fuzz (AST parsing, race detection)
- 🔄 Static analysis with gosec
- 🔄 SAST scanning in CI (CodeQL, Snyk)
- 🔄 Comparison testing against Go's official race detector

## Security Disclosure History

No security vulnerabilities have been reported or fixed yet (project is production-ready since v0.2.0).

When vulnerabilities are addressed, they will be listed here with:
- **CVE ID** (if assigned)
- **Affected versions**
- **Fixed in version**
- **Severity** (Critical/High/Medium/Low)
- **Credit** to reporter

## Security Contact

- **GitHub Security Advisory**: https://github.com/kolkov/racedetector/security/advisories/new
- **Public Issues** (for non-sensitive bugs): https://github.com/kolkov/racedetector/issues
- **Discussions**: https://github.com/kolkov/racedetector/discussions

## Bug Bounty Program

racedetector does not currently have a bug bounty program. We rely on responsible disclosure from the security community.

If you report a valid security vulnerability:
- ✅ Public credit in security advisory (if desired)
- ✅ Acknowledgment in CHANGELOG
- ✅ Recognition in README
- ✅ Priority review and quick fix
- ✅ Our gratitude!

## Threat Model

**Trust Assumptions**:
- Users trust the Go source code they instrument
- Users trust the Go toolchain (compiler, runtime)
- racedetector operates on the same trust level as `go build`

**Out of Scope**:
- Malicious Go compiler
- Compromised development machine
- Supply chain attacks on Go toolchain
- Physical access to developer machine

**In Scope**:
- Malicious Go source files (should not compromise racedetector)
- Untrusted third-party packages (should not exploit instrumentation)
- Resource exhaustion attacks (memory, CPU)
- Information disclosure via race reports

---

**Thank you for helping keep racedetector secure!** 🔒

*Security is a journey, not a destination. We continuously improve our security posture with each release.*
