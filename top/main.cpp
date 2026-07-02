#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <sys/ioctl.h>
#include <sys/types.h>

namespace constants {

constexpr int kPidColumnWidth = 5;
constexpr int kUserColumnWidth = 8;
constexpr int kPriorityColumnWidth = 3;
constexpr int kNiceColumnWidth = 3;
constexpr int kVirtColumnWidth = 7;
constexpr int kResColumnWidth = 7;
constexpr int kSColumnWidth = 1;
constexpr int kCpuColumnWidth = 5;
constexpr int kMemColumnWidth = 5;
constexpr int kTimeColumnWidth = 10;

constexpr int kDefaultMaxCommandWidth = 60;
constexpr int kMinimumCommandWidth = 8;
constexpr int kMaxPrintedRows = 30;

constexpr int kPasswdBufferSize = 4096;

constexpr int64_t kHundred = 100;
constexpr int64_t kSecondsPerMinute = 60;
constexpr int64_t kMinutesPerHour = 60;
constexpr int64_t kSecondsPerHour = 3600;
constexpr int kTwoDigitWidth = 2;

constexpr int64_t kBytesPerKilobyte = 1024;
constexpr int64_t kKilobytesPerMegabyte = 1024;
constexpr int64_t kKilobytesPerGigabyte = 1024 * 1024;

}  // namespace constants

struct ProcessInfo {
    pid_t pid = 0;
    std::string user;
    int64_t priority = 0;
    int64_t nice = 0;
    int64_t virt_kb = 0;
    int64_t rss_kb = 0;
    char state = '?';
    double cpu_percent = 0.0;
    double mem_percent = 0.0;
    int64_t total_ticks = 0;
    std::string time_str;
    std::string command;
};

bool IsNumber(const std::string& input) {
    return !input.empty() && std::all_of(input.begin(), input.end(), isdigit);
}

int64_t ReadTotalMemoryKb() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) {
        return 0;
    }
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.starts_with("MemTotal:")) {
            std::istringstream iss(line);
            std::string label;
            int64_t value = 0;
            std::string unit;
            iss >> label >> value >> unit;
            return value;
        }
    }
    return 0;
}

std::string FormatTimeFromTicks(int64_t ticks, int64_t ticks_per_second) {
    if (ticks_per_second <= 0) {
        return "0:00.00";
    }
    int64_t total_centiseconds = (ticks * constants::kHundred) / ticks_per_second;
    int64_t centiseconds = total_centiseconds % constants::kHundred;
    int64_t total_seconds = total_centiseconds / constants::kHundred;
    int64_t seconds = total_seconds % constants::kSecondsPerMinute;
    int64_t minutes = (total_seconds / constants::kSecondsPerMinute) % constants::kMinutesPerHour;
    int64_t hours = total_seconds / constants::kSecondsPerHour;

    std::ostringstream out;
    if (hours > 0) {
        out << hours << ':' << std::setw(constants::kTwoDigitWidth) << std::setfill('0') << minutes;
    } else {
        out << minutes;
    }
    out << ':' << std::setw(constants::kTwoDigitWidth) << std::setfill('0') << seconds << '.'
        << std::setw(constants::kTwoDigitWidth) << std::setfill('0') << centiseconds;
    return out.str();
}

std::string ReadUserName(uid_t uid) {
    struct passwd pwd;
    struct passwd* result = nullptr;
    char buffer[constants::kPasswdBufferSize];

    if (getpwuid_r(uid, &pwd, buffer, sizeof(buffer), &result) == 0 && result != nullptr &&
        pwd.pw_name != nullptr) {
        return std::string(pwd.pw_name);
    }
    return std::to_string(static_cast<uint64_t>(uid));
}

std::optional<uid_t> ReadProcessUid(pid_t pid) {
    std::ostringstream path;
    path << "/proc/" << pid << "/status";
    std::ifstream status_file(path.str());
    if (!status_file) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(status_file, line)) {
        if (line.starts_with("Uid:")) {
            uint64_t uid_value = 0;
            std::istringstream iss(line);
            std::string label;
            if (iss >> label >> uid_value) {
                return static_cast<uid_t>(uid_value);
            }
            break;
        }
    }
    return std::nullopt;
}

bool ReadProcessStat(pid_t pid, int64_t page_size, int64_t ticks_per_second, ProcessInfo* info) {
    std::ostringstream path;
    path << "/proc/" << pid << "/stat";
    std::ifstream stat_file(path.str());
    if (!stat_file) {
        return false;
    }
    std::string line;
    if (!std::getline(stat_file, line)) {
        return false;
    }

    std::size_t lparen = line.find('(');
    std::size_t rparen = line.rfind(')');
    if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) {
        return false;
    }

    std::string comm = line.substr(lparen + 1, rparen - lparen - 1);
    std::string after = line.substr(rparen + 2);

    std::istringstream iss(after);

    char state;
    int64_t ppid;
    int64_t pgrp;
    int64_t session;
    int64_t tty_nr;
    int64_t tpgid;
    uint64_t flags;
    uint64_t minflt;
    uint64_t cminflt;
    uint64_t majflt;
    uint64_t cmajflt;
    int64_t utime;
    int64_t stime;
    int64_t cutime;
    int64_t cstime;
    int64_t priority;
    int64_t nice;
    int64_t num_threads;
    int64_t itrealvalue;
    int64_t starttime;
    uint64_t vsize;
    int64_t rss;

    iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags >> minflt >> cminflt >>
        majflt >> cmajflt >> utime >> stime >> cutime >> cstime >> priority >> nice >>
        num_threads >> itrealvalue >> starttime >> vsize >> rss;
    if (!iss) {
        return false;
    }

    info->pid = pid;
    info->state = state;
    info->priority = priority;
    info->nice = nice;
    info->virt_kb = static_cast<int64_t>(vsize) / constants::kBytesPerKilobyte;
    info->rss_kb = static_cast<int64_t>(rss) * page_size / constants::kBytesPerKilobyte;
    info->total_ticks = utime + stime;
    info->time_str = FormatTimeFromTicks(info->total_ticks, ticks_per_second);
    info->command = comm;
    return true;
}

std::vector<pid_t> ListPids() {
    std::vector<pid_t> result;
    DIR* dir = opendir("/proc");
    if (dir == nullptr) {
        return result;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK) {
            continue;
        }
        std::string name(entry->d_name);
        if (!IsNumber(name)) {
            continue;
        }
        pid_t pid = static_cast<pid_t>(std::strtol(name.data(), nullptr, 10));
        if (pid > 0) {
            result.push_back(pid);
        }
    }
    closedir(dir);
    return result;
}

int GetTerminalWidth() {
    if (isatty(STDOUT_FILENO) == 0) {
        return 0;
    }
    struct winsize window_size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) != 0 || window_size.ws_col <= 0) {
        return 0;
    }
    return window_size.ws_col;
}

double CalculateCpuPercent(int64_t delta_ticks, int64_t prev_ticks, int64_t ticks_per_second) {
    int64_t delta_proc = (delta_ticks >= prev_ticks) ? (delta_ticks - prev_ticks) : 0;
    return static_cast<double>(delta_proc) * 100.0 / static_cast<double>(ticks_per_second);
}

double CalculateMemPercent(int64_t rss_kb, int64_t total_memory_kb) {
    if (total_memory_kb <= 0) {
        return 0.0;
    }
    return static_cast<double>(rss_kb) * 100.0 / static_cast<double>(total_memory_kb);
}

std::string FormatMemoryAmount(int64_t kilobytes) {
    std::ostringstream out;
    out << std::fixed;

    if (kilobytes >= constants::kKilobytesPerGigabyte) {
        double gigabytes = static_cast<double>(kilobytes) / constants::kKilobytesPerGigabyte;
        out << std::setprecision(gigabytes < 10.0 ? 1 : 0) << gigabytes << 'g';
    } else if (kilobytes >= constants::kKilobytesPerMegabyte) {
        double megabytes = static_cast<double>(kilobytes) / constants::kKilobytesPerMegabyte;
        out << std::setprecision(megabytes < 10.0 ? 1 : 0) << megabytes << 'm';
    } else {
        out << kilobytes << 'k';
    }
    return out.str();
}

void PrintHeader() {
    std::cout << std::right << std::setw(constants::kPidColumnWidth) << "PID" << ' ';
    std::cout << std::left << std::setw(constants::kUserColumnWidth) << "USER";
    std::cout << std::right << ' ';
    std::cout << std::setw(constants::kPriorityColumnWidth) << "PR" << ' '
              << std::setw(constants::kNiceColumnWidth) << "NI" << ' ';
    std::cout << std::setw(constants::kVirtColumnWidth) << "VIRT" << ' '
              << std::setw(constants::kResColumnWidth) << "RES" << ' ';
    std::cout << 'S' << ' ';
    std::cout << std::setw(constants::kCpuColumnWidth) << "%CPU" << ' '
              << std::setw(constants::kMemColumnWidth) << "%MEM" << ' ';
    std::cout << std::setw(constants::kTimeColumnWidth) << "TIME+" << ' ';
    std::cout << "COMMAND" << '\n';
}

std::pair<std::vector<ProcessInfo>, std::unordered_map<pid_t, int64_t>> CollectProcesses(
    const std::vector<pid_t>& process_ids,
    const std::unordered_map<pid_t, int64_t>& prev_process_ticks, int64_t page_size,
    int64_t ticks_per_second, int64_t total_memory_kb) {
    std::vector<ProcessInfo> processes;
    processes.reserve(process_ids.size());
    std::unordered_map<pid_t, int64_t> new_prev_ticks;

    for (pid_t pid : process_ids) {
        ProcessInfo info;
        if (!ReadProcessStat(pid, page_size, ticks_per_second, &info)) {
            continue;
        }

        std::optional<uid_t> uid_opt = ReadProcessUid(pid);
        if (!uid_opt.has_value()) {
            continue;
        }
        info.user = ReadUserName(*uid_opt);

        auto it = prev_process_ticks.find(pid);
        int64_t prev_ticks = (it != prev_process_ticks.end()) ? it->second : 0;
        info.cpu_percent = CalculateCpuPercent(info.total_ticks, prev_ticks, ticks_per_second);
        info.mem_percent = CalculateMemPercent(info.rss_kb, total_memory_kb);

        new_prev_ticks[pid] = info.total_ticks;
        processes.push_back(info);
    }

    std::sort(processes.begin(), processes.end(),
              [](const ProcessInfo& lhs, const ProcessInfo& rhs) {
                  if (lhs.cpu_percent != rhs.cpu_percent) {
                      return lhs.cpu_percent > rhs.cpu_percent;
                  }
                  return lhs.pid < rhs.pid;
              });

    if (processes.size() > constants::kMaxPrintedRows) {
        processes.resize(constants::kMaxPrintedRows);
    }

    return {std::move(processes), std::move(new_prev_ticks)};
}

void PrintProcessesTable(const std::vector<ProcessInfo>& processes) {
    PrintHeader();

    int terminal_width = GetTerminalWidth();
    int fixed_columns_width = constants::kPidColumnWidth + 1 + constants::kUserColumnWidth + 1 +
                              constants::kPriorityColumnWidth + 1 + constants::kNiceColumnWidth +
                              1 + constants::kVirtColumnWidth + 1 + constants::kResColumnWidth + 1 +
                              constants::kSColumnWidth + 1 + constants::kCpuColumnWidth + 1 +
                              constants::kMemColumnWidth + 1 + constants::kTimeColumnWidth + 1;

    int max_command_width = (terminal_width > 0 &&
                             terminal_width - fixed_columns_width > constants::kMinimumCommandWidth)
                                ? terminal_width - fixed_columns_width
                                : constants::kDefaultMaxCommandWidth;

    for (const auto& process : processes) {
        std::cout << std::setw(constants::kPidColumnWidth) << process.pid << ' ';

        if (process.user.size() > constants::kUserColumnWidth) {
            std::cout << std::left << std::setw(constants::kUserColumnWidth)
                      << process.user.substr(0, constants::kUserColumnWidth) << std::right;
        } else {
            std::cout << std::left << std::setw(constants::kUserColumnWidth) << process.user
                      << std::right;
        }
        std::cout << ' ';

        std::cout << std::setw(constants::kPriorityColumnWidth) << process.priority << ' '
                  << std::setw(constants::kNiceColumnWidth) << process.nice << ' ';

        std::cout << std::setw(constants::kVirtColumnWidth) << FormatMemoryAmount(process.virt_kb)
                  << ' ' << std::setw(constants::kResColumnWidth)
                  << FormatMemoryAmount(process.rss_kb) << ' ';

        std::cout << process.state << ' ';

        std::cout << std::fixed << std::setprecision(1) << std::setw(constants::kCpuColumnWidth)
                  << process.cpu_percent << ' ' << std::setw(constants::kMemColumnWidth)
                  << process.mem_percent << ' ';

        std::cout << std::setw(constants::kTimeColumnWidth) << process.time_str << ' ';

        if (static_cast<int>(process.command.size()) > max_command_width) {
            if (max_command_width > 3) {
                std::cout << process.command.substr(0, max_command_width - 3) << "...";
            } else {
                std::cout << process.command.substr(0, max_command_width);
            }
        } else {
            std::cout << process.command;
        }
        std::cout << '\n';
    }
}

int main() {
    int64_t page_size = sysconf(_SC_PAGESIZE);
    int64_t ticks_per_second = sysconf(_SC_CLK_TCK);
    int64_t total_memory_kb = ReadTotalMemoryKb();

    if (page_size <= 0 || ticks_per_second <= 0 || total_memory_kb <= 0) {
        std::cerr << "failed to read system parameters\n";
        return 1;
    }

    bool is_tty = isatty(STDOUT_FILENO) != 0;

    std::unordered_map<pid_t, int64_t> prev_process_ticks;
    std::vector<pid_t> initial_pids = ListPids();
    for (const auto& pid : initial_pids) {
        ProcessInfo info;
        if (ReadProcessStat(pid, page_size, ticks_per_second, &info)) {
            prev_process_ticks[pid] = info.total_ticks;
        }
    }

    while (true) {
        sleep(1);

        std::vector<pid_t> process_ids = ListPids();
        auto [processes, new_prev_ticks] = CollectProcesses(
            process_ids, prev_process_ticks, page_size, ticks_per_second, total_memory_kb);

        if (is_tty) {
            std::cout << "\033[H\033[J";
        }
        PrintProcessesTable(processes);
        std::cout.flush();

        prev_process_ticks = std::move(new_prev_ticks);

        if (!is_tty) {
            break;
        }
    }
}
