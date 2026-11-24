# Contributing to Pure-Go Race Detector

Thank you for your interest in contributing to the Pure-Go Race Detector project!

## 🎯 Project Status

**Status:** Production-Ready
**Goal:** Official Go toolchain integration
**See:** [CHANGELOG.md](CHANGELOG.md) for version history and [ROADMAP.md](ROADMAP.md) for upcoming features

## 🐛 Reporting Issues

Please open an issue on GitHub with:
- Go version (`go version`)
- Operating system (Linux, macOS, Windows)
- Steps to reproduce
- Expected vs actual behavior
- Error messages (if any)

**Search existing issues first** to avoid duplicates!

## 💡 Feature Requests

We welcome feature requests! Please provide:
- Use case description
- Expected behavior
- Why this feature would benefit the community

**Current Priorities:**
See [ROADMAP.md](ROADMAP.md) for detailed development priorities and upcoming features.

## 🧪 Testing

We're actively looking for testers! Help us by:
- **Testing in diverse environments** (cloud, embedded, containers)
- **Reporting edge cases** you discover
- **Sharing use case stories** (success or failure)
- **Performance feedback** from real-world projects

This is the **most valuable contribution** at this stage!

## 📢 Spread the Word

Help us get community support for Go integration:
- ⭐ Star the repository
- Share on Twitter, Reddit, HN, Go forums
- Blog about your experience
- Present at Go meetups

**More stars = stronger case for official Go toolchain integration!**

## 🔮 Pull Requests

We welcome code contributions!

**We accept PRs for:**
- Bug fixes
- Performance optimizations
- Documentation improvements
- Test cases
- Additional platform support
- Edge case handling

**Before submitting:**
- Read this guide thoroughly
- Check existing issues/PRs
- Follow code quality standards
- Add tests for new features

## 🛠️ Development Setup

For code contributions:

```bash
# Clone repository
git clone https://github.com/kolkov/racedetector.git
cd racedetector

# Install tool
go install ./cmd/racedetector

# Verify installation
racedetector --version

# Run tests
go test ./internal/... ./race/... ./cmd/...

# Run linter
golangci-lint run ./internal/... ./race/... ./cmd/...
```

## 📋 Code Quality Standards (For Future PRs)

When submitting PRs:

1. **Follow existing code style:**
   - Run `go fmt ./...`
   - Pass `golangci-lint` checks
   - Add tests for new features

2. **Keep PRs focused:**
   - One feature/fix per PR
   - Clear commit messages (conventional commits)
   - Update documentation if adding features

3. **Ensure tests pass:**
   - All existing tests must pass
   - Coverage should not decrease
   - Add tests for bug fixes

## 📚 Learning Resources

**Public Documentation:**
- [README.md](README.md) - Project overview
- [INSTALLATION.md](docs/INSTALLATION.md) - Installation guide
- [USAGE_GUIDE.md](docs/USAGE_GUIDE.md) - Usage examples
- [ROADMAP.md](ROADMAP.md) - Development roadmap
- [CHANGELOG.md](CHANGELOG.md) - Release history

**Research:**
- FastTrack paper (PLDI 2009): [Link](https://users.soe.ucsc.edu/~cormac/papers/pldi09.pdf)
- Go race detector design: [Link](https://go.dev/blog/race-detector)

## 💬 Commit Message Format

Follow conventional commits:

```
feat: Add new feature
fix: Fix bug
docs: Update documentation
test: Add tests
refactor: Refactor code
perf: Performance improvement
chore: Maintenance tasks
```

Examples:
```
feat: Add support for ARM64 architecture
fix: Correct stack trace capture on Windows
docs: Add example for channel synchronization
test: Add test case for select statement race
perf: Optimize shadow memory lookup
```

## ❓ Questions?

- **GitHub Discussions** - Ask questions, share ideas
- **GitHub Issues** - Report bugs, request features
- **README.md** - Check FAQ section first

## 🙏 Thank You!

Every contribution helps make race detection better for the Go community!

**Your testing, feedback, and stars are invaluable for our goal of official Go integration.**

---

*Contributing guidelines for Pure-Go Race Detector*
