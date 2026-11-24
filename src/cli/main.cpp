#include "program_parser.hpp"
#include "report.hpp"

#include "model/causal_trace.hpp"
#include "model/checker.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

class UsageError : public std::runtime_error {
public:
    explicit UsageError(const std::string& message) : std::runtime_error(message) {}
};

enum class Explorer { Naive, Dpor };

struct CheckCommand {
    std::string program_path;
    Explorer explorer{Explorer::Dpor};
    std::size_t max_schedules{100000};
    std::size_t step_bound{model::ModelChecker::kDefaultStepBound};
    model::MemoryModel memory_model{model::MemoryModel::SC};
};

struct ReplayCommand {
    std::string program_path;
    std::string schedule_path;
    model::MemoryModel memory_model{model::MemoryModel::SC};
};

struct ExplainCommand {
    std::string program_path;
    std::string schedule_path;
    std::size_t from{0};
    std::size_t to{0};
    bool has_from{false};
    bool has_to{false};
    std::size_t max_events{0};
    model::MemoryModel memory_model{model::MemoryModel::SC};
};

std::size_t parse_size_token(const std::string& token, const std::string& description) {
    if (token.empty()) {
        throw UsageError("missing " + description);
    }

    std::uint64_t value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') {
            throw UsageError("invalid " + description);
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::size_t>::max()) {
            throw UsageError(description + " is too large");
        }
    }
    if (value == 0) {
        throw UsageError(description + " must be greater than zero");
    }
    return static_cast<std::size_t>(value);
}

std::size_t parse_index_token(const std::string& token, const std::string& description) {
    if (token.empty()) {
        throw UsageError("missing " + description);
    }
    std::uint64_t value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') {
            throw UsageError("invalid " + description);
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::size_t>::max()) {
            throw UsageError(description + " is too large");
        }
    }
    return static_cast<std::size_t>(value);
}

Explorer parse_explorer(const std::string& value) {
    if (value == "naive") {
        return Explorer::Naive;
    }
    if (value == "dpor") {
        return Explorer::Dpor;
    }
    throw UsageError("invalid explorer");
}

model::MemoryModel parse_memory_model(const std::string& value) {
    if (value == "sc") {
        return model::MemoryModel::SC;
    }
    if (value == "tso") {
        return model::MemoryModel::TSO;
    }
    if (value == "pso") {
        return model::MemoryModel::PSO;
    }
    throw UsageError("invalid memory model");
}

CheckCommand parse_check_command(int argc, char** argv) {
    if (argc < 3) {
        throw UsageError("usage: dpor check <program.dpor> [--explorer naive|dpor] [--memory-model sc|tso|pso] [--max-schedules N] [--step-bound N]");
    }

    CheckCommand command;
    command.program_path = argv[2];
    int index = 3;
    while (index < argc) {
        const std::string flag = argv[index];
        if (flag == "--explorer") {
            if (index + 1 >= argc) {
                throw UsageError("missing explorer");
            }
            command.explorer = parse_explorer(argv[index + 1]);
            index += 2;
        } else if (flag == "--memory-model") {
            if (index + 1 >= argc) {
                throw UsageError("missing memory model");
            }
            command.memory_model = parse_memory_model(argv[index + 1]);
            index += 2;
        } else if (flag == "--max-schedules") {
            if (index + 1 >= argc) {
                throw UsageError("missing max schedules");
            }
            command.max_schedules = parse_size_token(argv[index + 1], "max schedules");
            index += 2;
        } else if (flag == "--step-bound") {
            if (index + 1 >= argc) {
                throw UsageError("missing step bound");
            }
            command.step_bound = parse_size_token(argv[index + 1], "step bound");
            index += 2;
        } else {
            throw UsageError("unknown option");
        }
    }
    return command;
}

ReplayCommand parse_replay_command(int argc, char** argv) {
    if (argc < 5) {
        throw UsageError("usage: dpor replay <program.dpor> --schedule <schedule-file> [--memory-model sc|tso|pso]");
    }
    ReplayCommand command;
    command.program_path = argv[2];
    int index = 3;
    while (index < argc) {
        const std::string flag = argv[index];
        if (flag == "--schedule") {
            if (index + 1 >= argc) {
                throw UsageError("missing schedule file");
            }
            command.schedule_path = argv[index + 1];
            index += 2;
        } else if (flag == "--memory-model") {
            if (index + 1 >= argc) {
                throw UsageError("missing memory model");
            }
            command.memory_model = parse_memory_model(argv[index + 1]);
            index += 2;
        } else {
            throw UsageError("unknown option");
        }
    }
    if (command.schedule_path.empty()) {
        throw UsageError("usage: dpor replay <program.dpor> --schedule <schedule-file> [--memory-model sc|tso|pso]");
    }
    return command;
}

ExplainCommand parse_explain_command(int argc, char** argv) {
    if (argc < 3) {
        throw UsageError(
            "usage: dpor explain <program.dpor> --schedule <schedule-file> "
            "--from N --to N [--max-events N] [--memory-model sc|tso|pso]");
    }

    ExplainCommand command;
    command.program_path = argv[2];
    int index = 3;
    while (index < argc) {
        const std::string flag = argv[index];
        if (flag == "--schedule") {
            if (index + 1 >= argc) {
                throw UsageError("missing schedule file");
            }
            command.schedule_path = argv[index + 1];
            index += 2;
        } else if (flag == "--from") {
            if (index + 1 >= argc) {
                throw UsageError("missing --from id");
            }
            command.from = parse_index_token(argv[index + 1], "--from id");
            command.has_from = true;
            index += 2;
        } else if (flag == "--to") {
            if (index + 1 >= argc) {
                throw UsageError("missing --to id");
            }
            command.to = parse_index_token(argv[index + 1], "--to id");
            command.has_to = true;
            index += 2;
        } else if (flag == "--max-events") {
            if (index + 1 >= argc) {
                throw UsageError("missing max events");
            }
            command.max_events = parse_index_token(argv[index + 1], "max events");
            index += 2;
        } else if (flag == "--memory-model") {
            if (index + 1 >= argc) {
                throw UsageError("missing memory model");
            }
            command.memory_model = parse_memory_model(argv[index + 1]);
            index += 2;
        } else {
            throw UsageError("unknown option");
        }
    }

    if (command.schedule_path.empty() || !command.has_from || !command.has_to) {
        throw UsageError(
            "usage: dpor explain <program.dpor> --schedule <schedule-file> "
            "--from N --to N [--max-events N] [--memory-model sc|tso|pso]");
    }
    return command;
}

int run_check(const CheckCommand& command) {
    const model::Program program = cli::parse_program_file(command.program_path);
    const model::ModelChecker checker(program, command.step_bound, command.memory_model);
    const model::CheckResult result = command.explorer == Explorer::Naive
        ? checker.explore_naive(command.max_schedules)
        : checker.explore_dpor(command.max_schedules);
    cli::print_report(std::cout, program, result, command.memory_model, command.step_bound);
    return cli::has_bug(result) ? 1 : 0;
}

int run_replay(const ReplayCommand& command) {
    const model::Program program = cli::parse_program_file(command.program_path);
    const model::Schedule schedule = cli::parse_schedule_file(command.schedule_path);
    const model::ModelChecker checker(program, model::ModelChecker::kDefaultStepBound, command.memory_model);
    const model::CheckResult result = checker.replay(schedule);
    cli::print_report(std::cout, program, result, command.memory_model);
    return cli::has_bug(result) ? 1 : 0;
}

const char* causal_action_name(model::ActionKind kind) {
    switch (kind) {
    case model::ActionKind::Set: return "set";
    case model::ActionKind::Label: return "label";
    case model::ActionKind::BranchNonzero: return "bnz";
    case model::ActionKind::Assert: return "assert";
    case model::ActionKind::Read: return "read";
    case model::ActionKind::Write: return "write";
    case model::ActionKind::AtomicLoad: return "atomic_load";
    case model::ActionKind::AtomicStore: return "atomic_store";
    case model::ActionKind::AtomicRmw: return "atomic_rmw";
    case model::ActionKind::CompareExchange: return "cas";
    case model::ActionKind::Fence: return "fence";
    case model::ActionKind::Flush: return "flush";
    case model::ActionKind::Lock: return "lock";
    case model::ActionKind::TryLock: return "try_lock";
    case model::ActionKind::Unlock: return "unlock";
    case model::ActionKind::Spawn: return "spawn";
    case model::ActionKind::Join: return "join";
    case model::ActionKind::Wait: return "wait";
    case model::ActionKind::Signal: return "signal";
    case model::ActionKind::Broadcast: return "broadcast";
    case model::ActionKind::Yield: return "yield";
    case model::ActionKind::RLock: return "rlock";
    case model::ActionKind::RUnlock: return "runlock";
    case model::ActionKind::WLock: return "wlock";
    case model::ActionKind::WUnlock: return "wunlock";
    case model::ActionKind::SemPost: return "sem_post";
    case model::ActionKind::SemWait: return "sem_wait";
    case model::ActionKind::BarrierWait: return "barrier_wait";
    case model::ActionKind::Upgrade: return "upgrade";
    case model::ActionKind::Downgrade: return "downgrade";
    case model::ActionKind::TimedWait: return "timedwait";
    }
    return "unknown";
}

int run_explain(const ExplainCommand& command) {
    const model::Program program = cli::parse_program_file(command.program_path);
    const model::Schedule schedule = cli::parse_schedule_file(command.schedule_path);
    const model::ModelChecker checker(program,
                                      model::ModelChecker::kDefaultStepBound,
                                      command.memory_model);
    const model::CausalTrace trace = checker.causal_trace(schedule, command.max_events);
    // explain performs bounds checking and throws std::out_of_range here.
    const std::vector<std::size_t> path = trace.explain(command.from, command.to);

    std::cout << "CAUSAL events=" << trace.events.size()
              << " edges=" << trace.edges.size()
              << " cancelled=" << (trace.cancelled ? 1 : 0) << '\n';

    for (const model::CausalEvent& event : trace.events) {
        std::cout << "EVENT " << event.id
                  << " thread=" << event.endpoint.thread
                  << " action=" << causal_action_name(event.effective_action.kind)
                  << " endpoint=" << event.endpoint.thread << ':'
                  << event.endpoint.action_index
                  << " clock=";
        for (std::size_t i = 0; i < event.clock_after.size(); ++i) {
            if (i != 0) {
                std::cout << ',';
            }
            std::cout << event.clock_after[i];
        }
        if (event.barrier_generation.has_value()) {
            std::cout << " barrier_generation=" << *event.barrier_generation;
        }
        if (event.timed_wait_occurrence.has_value()) {
            std::cout << " timed_wait="
                      << (event.timed_wait_occurrence->transition ==
                                  model::TimedWaitTransition::Park
                              ? "Park"
                              : "Timeout")
                      << '#' << event.timed_wait_occurrence->episode;
        }
        std::cout << '\n';
    }

    std::cout << "EXPLAIN from=" << command.from << " to=" << command.to
              << " edge_count=" << path.size() << '\n';
    std::cout << "PATH";
    for (const std::size_t edge_index : path) {
        std::cout << ' ' << edge_index;
    }
    std::cout << '\n';

    std::cout << "PATH_EVENTS";
    if (!path.empty()) {
        std::cout << ' ' << trace.edges[path.front()].from;
        for (const std::size_t edge_index : path) {
            std::cout << ' ' << trace.edges[edge_index].to;
        }
    }
    std::cout << '\n';

    for (const std::size_t edge_index : path) {
        const model::CausalEdge& edge = trace.edges.at(edge_index);
        std::cout << "EDGE " << edge_index << ' ' << edge.from << ' ' << edge.to
                  << ' ' << edge.reason << '\n';
    }
    if (path.empty()) {
        std::cout << "NO_PATH\n";
    }
    return 0;
}

void print_parse_error(const cli::ParseError& error) {
    if (error.line() == 0) {
        std::cerr << "parse error: " << error.what() << '\n';
    } else {
        std::cerr << "parse error at line " << error.line() << ": " << error.what() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            throw UsageError("usage: dpor <check|replay|explain> ...");
        }

        const std::string subcommand = argv[1];
        if (subcommand == "check") {
            return run_check(parse_check_command(argc, argv));
        }
        if (subcommand == "replay") {
            return run_replay(parse_replay_command(argc, argv));
        }
        if (subcommand == "explain") {
            return run_explain(parse_explain_command(argc, argv));
        }
        throw UsageError("usage: dpor <check|replay|explain> ...");
    } catch (const cli::ParseError& error) {
        print_parse_error(error);
        return 2;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const UsageError& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
