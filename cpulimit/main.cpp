#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <signal.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include <sys/time.h>

namespace {

namespace constants {

constexpr int kPeriodMs = 1000;
constexpr int kCheckIntervalMs = 10;
constexpr int kMicrosecondsPerMs = 1000;
constexpr int kMillisecondsPerSecond = 1000;
constexpr int kPercentBase = 100;

}  // namespace constants

struct ProcessInfo {
    int64_t period_start_ticks = 0;
    bool stopped_by_us = false;
};

struct Config {
    int ncpu = 0;
    int limit_percent = -1;
    pid_t target_pid = 0;
    std::string exec_name;
    bool use_pid = false;
    bool use_exec = false;
};

bool IsNumber(const std::string& str) {
    return !str.empty() && std::all_of(str.begin(), str.end(), isdigit);
}

std::vector<pid_t> ListPids() {
    std::vector<pid_t> result;
    DIR* dir = opendir("/proc");
    if (dir == nullptr) {
        return result;
    }
    dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (IsNumber(name)) {
            pid_t pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
            if (pid > 0) {
                result.push_back(pid);
            }
        }
    }
    closedir(dir);
    return result;
}

std::string ReadStatFields(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!file || !std::getline(file, line)) {
        return "";
    }
    size_t pos = line.rfind(')');
    if (pos == std::string::npos || pos + 2 >= line.size()) {
        return "";
    }
    return line.substr(pos + 2);
}

int64_t ReadCpuTicks(pid_t pid) {
    std::string fields = ReadStatFields(pid);
    if (fields.empty()) {
        return -1;
    }
    std::istringstream iss(fields);
    char state = 0;
    int64_t skip = 0;
    int64_t utime = 0;
    int64_t stime = 0;
    iss >> state >> skip;
    for (int i = 0; i < 9; ++i) {
        iss >> skip;
    }
    iss >> utime >> stime;
    if (!iss) {
        return -1;
    }
    return utime + stime;
}

std::string ReadExecutableName(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (!file || !std::getline(file, name)) {
        return "";
    }
    return name;
}

pid_t ReadPpid(pid_t pid) {
    std::string fields = ReadStatFields(pid);
    if (fields.empty()) {
        return -1;
    }
    std::istringstream iss(fields);
    char state = 0;
    pid_t ppid = 0;
    iss >> state >> ppid;
    if (!iss) {
        return -1;
    }
    return ppid;
}

std::map<pid_t, pid_t> GetParentMap(const std::vector<pid_t>& all_pids) {
    std::map<pid_t, pid_t> parent_map;
    for (pid_t pid : all_pids) {
        pid_t ppid = ReadPpid(pid);
        if (ppid > 0) {
            parent_map[pid] = ppid;
        }
    }
    return parent_map;
}

std::map<pid_t, std::vector<pid_t>> GetChildrenMap(const std::map<pid_t, pid_t>& parent_map) {
    std::map<pid_t, std::vector<pid_t>> children_map;
    for (const auto& [pid, ppid] : parent_map) {
        children_map[ppid].push_back(pid);
    }
    return children_map;
}

void CollectChildren(pid_t parent, const std::map<pid_t, std::vector<pid_t>>& children_map,
                     std::vector<pid_t>& result) {
    auto it = children_map.find(parent);
    if (it == children_map.end()) {
        return;
    }
    for (pid_t child : it->second) {
        result.push_back(child);
        CollectChildren(child, children_map, result);
    }
}

std::vector<pid_t> GetDescendants(pid_t parent, const std::vector<pid_t>& all_pids) {
    std::map<pid_t, pid_t> parent_map = GetParentMap(all_pids);
    std::map<pid_t, std::vector<pid_t>> children_map = GetChildrenMap(parent_map);
    std::vector<pid_t> descendants;
    CollectChildren(parent, children_map, descendants);
    return descendants;
}

std::vector<pid_t> FindProcessesByName(const std::string& exec_name) {
    std::vector<pid_t> result;
    std::vector<pid_t> all_pids = ListPids();
    std::unordered_set<pid_t> added;

    for (pid_t pid : all_pids) {
        std::string name = ReadExecutableName(pid);
        if (name != exec_name || added.count(pid) != 0) {
            continue;
        }

        result.push_back(pid);
        added.insert(pid);

        std::vector<pid_t> descendants = GetDescendants(pid, all_pids);
        for (pid_t descendant : descendants) {
            if (added.count(descendant) == 0) {
                result.push_back(descendant);
                added.insert(descendant);
            }
        }
    }
    return result;
}

bool ProcessExists(pid_t pid) {
    return kill(pid, 0) == 0;
}

void PrintUsageAndExit(int code, int ncpu) {
    std::cerr << "Usage: cpulimit [OPTIONS...] TARGET\n"
              << "  -l  percentage of cpu allowed from 0 to " << ncpu * constants::kPercentBase
              << " (required)\n"
              << "  -p  pid of the process\n"
              << "  -e  name of the executable program file\n"
              << std::endl;
    exit(code);
}

Config ParseArgs(int argc, char** argv, int ncpu) {
    Config config;
    config.ncpu = ncpu;

    const char* optstring = "hl:p:e:";
    int opt = getopt(argc, argv, optstring);

    while (opt != -1) {
        switch (opt) {
            case 'h':
                PrintUsageAndExit(0, ncpu);
                break;
            case 'l': {
                int limit_value = std::atoi(optarg);
                if (limit_value < 0 || limit_value > ncpu * constants::kPercentBase) {
                    std::cerr << "error: invalid value for -l\n";
                    PrintUsageAndExit(1, ncpu);
                }
                config.limit_percent = limit_value;
                break;
            }
            case 'p': {
                if (config.use_exec) {
                    std::cerr << "error: cannot use -p and -e together\n";
                    PrintUsageAndExit(1, ncpu);
                }
                pid_t pid = std::atoi(optarg);
                if (pid <= 0) {
                    std::cerr << "error: invalid PID\n";
                    PrintUsageAndExit(1, ncpu);
                }
                config.target_pid = pid;
                config.use_pid = true;
                break;
            }
            case 'e':
                if (config.use_pid) {
                    std::cerr << "error: cannot use -p and -e together\n";
                    PrintUsageAndExit(1, ncpu);
                }
                config.exec_name = optarg;
                config.use_exec = true;
                break;
            default:
                PrintUsageAndExit(1, ncpu);
        }
        opt = getopt(argc, argv, optstring);
    }

    if (config.limit_percent < 0) {
        std::cerr << "error: cpu limit is required\n";
        PrintUsageAndExit(1, ncpu);
    }

    if (!config.use_pid && !config.use_exec) {
        std::cerr << "error: specify either pid or executable name\n";
        PrintUsageAndExit(1, ncpu);
    }

    return config;
}

void ResumeProcesses(std::map<pid_t, ProcessInfo>& processes, const std::vector<pid_t>& pids) {
    for (pid_t pid : pids) {
        if (!ProcessExists(pid)) {
            continue;
        }
        auto it = processes.find(pid);
        if (it != processes.end() && it->second.stopped_by_us) {
            if (kill(pid, SIGCONT) == 0) {
                it->second.stopped_by_us = false;
            }
        }
    }
}

void StopProcesses(std::map<pid_t, ProcessInfo>& processes, const std::vector<pid_t>& pids) {
    for (pid_t pid : pids) {
        if (!ProcessExists(pid)) {
            continue;
        }
        auto it = processes.find(pid);
        if (it != processes.end() && !it->second.stopped_by_us) {
            if (kill(pid, SIGSTOP) == 0) {
                it->second.stopped_by_us = true;
            }
        }
    }
}

std::map<pid_t, ProcessInfo>* g_processes = nullptr;

void SignalHandler([[maybe_unused]] int sig) {
    if (g_processes == nullptr) {
        exit(0);
    }
    for (auto& [pid, info] : *g_processes) {
        if (ProcessExists(pid) && info.stopped_by_us) {
            kill(pid, SIGCONT);
        }
    }
    exit(0);
}

std::vector<pid_t> GetActivePids(const std::map<pid_t, ProcessInfo>& processes) {
    std::vector<pid_t> active_pids;
    for (const auto& [pid, info] : processes) {
        if (ProcessExists(pid)) {
            active_pids.push_back(pid);
        }
    }
    return active_pids;
}

int64_t CalculateElapsedMs(const timeval& start) {
    timeval now = {};
    gettimeofday(&now, nullptr);
    int64_t sec_diff = now.tv_sec - start.tv_sec;
    int64_t usec_diff = now.tv_usec - start.tv_usec;
    return sec_diff * constants::kMillisecondsPerSecond + usec_diff / constants::kMicrosecondsPerMs;
}

int64_t CalculateTotalUsedTicks(const std::map<pid_t, ProcessInfo>& processes,
                                const std::vector<pid_t>& active_pids) {
    int64_t total_used = 0;
    for (pid_t pid : active_pids) {
        auto it = processes.find(pid);
        if (it == processes.end()) {
            continue;
        }
        int64_t current_ticks = ReadCpuTicks(pid);
        if (current_ticks > 0 && it->second.period_start_ticks > 0) {
            total_used += current_ticks - it->second.period_start_ticks;
        }
    }
    return total_used;
}

void SleepMs(int64_t milliseconds) {
    if (milliseconds <= 0) {
        return;
    }
    timespec ts = {};
    ts.tv_sec = milliseconds / constants::kMillisecondsPerSecond;
    ts.tv_nsec = (milliseconds % constants::kMillisecondsPerSecond) *
                 constants::kMicrosecondsPerMs * constants::kMicrosecondsPerMs;
    nanosleep(&ts, nullptr);
}

void ResetPeriodTicks(std::map<pid_t, ProcessInfo>& processes,
                      const std::vector<pid_t>& active_pids) {
    for (pid_t pid : active_pids) {
        auto it = processes.find(pid);
        if (it != processes.end()) {
            it->second.period_start_ticks = ReadCpuTicks(pid);
        }
    }
}

std::map<pid_t, ProcessInfo> InitializeProcesses(const std::vector<pid_t>& target_pids) {
    std::map<pid_t, ProcessInfo> processes;
    for (pid_t pid : target_pids) {
        if (ProcessExists(pid)) {
            ProcessInfo info;
            info.period_start_ticks = ReadCpuTicks(pid);
            processes[pid] = info;
        }
    }
    return processes;
}

void RunLimitingLoop(std::map<pid_t, ProcessInfo>& processes, int64_t budget_ticks) {
    timeval period_start;
    gettimeofday(&period_start, nullptr);

    while (true) {
        std::vector<pid_t> active_pids = GetActivePids(processes);
        if (active_pids.empty()) {
            break;
        }

        int64_t elapsed_ms = CalculateElapsedMs(period_start);

        if (elapsed_ms >= constants::kPeriodMs) {
            ResumeProcesses(processes, active_pids);
            ResetPeriodTicks(processes, active_pids);
            gettimeofday(&period_start, nullptr);
            continue;
        }

        int64_t total_used = CalculateTotalUsedTicks(processes, active_pids);

        if (total_used >= budget_ticks) {
            StopProcesses(processes, active_pids);
            int64_t remaining_ms = constants::kPeriodMs - elapsed_ms;
            SleepMs(remaining_ms);
        } else {
            ResumeProcesses(processes, active_pids);
            SleepMs(constants::kCheckIntervalMs);
        }
    }

    std::vector<pid_t> final_pids = GetActivePids(processes);
    ResumeProcesses(processes, final_pids);
}

void LimitProcesses(const Config& config) {
    std::vector<pid_t> target_pids;
    if (config.use_pid) {
        target_pids.push_back(config.target_pid);
    } else {
        target_pids = FindProcessesByName(config.exec_name);
        if (target_pids.empty()) {
            std::cerr << "no processes found\n";
            return;
        }
    }

    std::map<pid_t, ProcessInfo> processes = InitializeProcesses(target_pids);
    if (processes.empty()) {
        std::cerr << "no valid processes to limit\n";
        return;
    }

    int64_t ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second <= 0) {
        std::cerr << "error: cannot get clock ticks per second\n";
        return;
    }

    double period_seconds =
        static_cast<double>(constants::kPeriodMs) / constants::kMillisecondsPerSecond;
    double limit_fraction = static_cast<double>(config.limit_percent) / constants::kPercentBase;
    int64_t budget_ticks = static_cast<int64_t>(limit_fraction * ticks_per_second * period_seconds);

    g_processes = &processes;
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    RunLimitingLoop(processes, budget_ticks);
    g_processes = nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    int ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) {
        std::cerr << "error: cannot determine number of CPUs\n";
        return 1;
    }

    Config config = ParseArgs(argc, argv, ncpu);
    LimitProcesses(config);
}
