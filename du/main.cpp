#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <memory>
#include <set>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

struct Config {
    bool summarize_only = false;
    bool all_files = false;
    bool dereference = false;
};

struct FileID {
    dev_t device;
    ino_t inode_number;

    bool operator<(const FileID& other) const {
        if (device != other.device) {
            return device < other.device;
        }
        return inode_number < other.inode_number;
    }
};

std::string JoinPath(const std::string& dir, const char* name) {
    if (dir.empty()) {
        return name == nullptr ? std::string() : std::string(name);
    }
    if (name == nullptr) {
        return dir;
    }
    if (dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

class DuDirectoryEntry {
public:
    DuDirectoryEntry() = default;

    DuDirectoryEntry(std::string path, const struct stat& st, uint64_t size = 0)
        : path_(std::move(path)), stat_buffer_(st), size_(size) {
    }

    const std::string& Path() const {
        return path_;
    }

    const struct stat& StatInfo() const {
        return stat_buffer_;
    }

    uint64_t Size() const {
        return size_;
    }

    void SetSize(uint64_t size) {
        size_ = size;
    }

private:
    std::string path_;
    struct stat stat_buffer_{};
    uint64_t size_ = 0;
};

struct DirDeleter {
    void operator()(DIR* dir) const {
        if (dir != nullptr) {
            closedir(dir);
        }
    }
};

class DirectoryIterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = DuDirectoryEntry;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    explicit DirectoryIterator(const std::string& path) : root_(path) {
        DIR* dir = opendir(path.data());
        if (dir != nullptr) {
            current_dir_ = std::shared_ptr<DIR>(dir, DirDeleter{});
            ++(*this);
        }
    }

    DirectoryIterator() = default;
    DirectoryIterator(const DirectoryIterator&) = default;
    DirectoryIterator& operator=(const DirectoryIterator&) = default;
    ~DirectoryIterator() = default;

    bool IsValid() const {
        return has_entry_;
    }

    reference operator*() const {
        return current_entry_;
    }

    pointer operator->() const {
        return &current_entry_;
    }

    const std::string& DirectoryPath() const {
        return root_;
    }

    uint64_t TotalSize() const {
        return total_size_;
    }

    void AddToTotal(uint64_t size) {
        total_size_ += size;
    }

    bool IsRoot() const {
        return is_root_;
    }

    void SetRoot(bool is_root) {
        is_root_ = is_root;
    }

    DirectoryIterator& operator++() {
        if (current_dir_ == nullptr) {
            has_entry_ = false;
            return *this;
        }

        has_entry_ = false;
        dirent* entry = nullptr;
        while ((entry = readdir(current_dir_.get())) != nullptr) {
            const char* name = entry->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            std::string full_path = JoinPath(root_, name);
            struct stat st{};
            if (lstat(full_path.data(), &st) != 0) {
                perror(full_path.data());
                continue;
            }
            current_entry_ =
                DuDirectoryEntry(std::move(full_path), st, static_cast<uint64_t>(st.st_size));
            has_entry_ = true;
            return *this;
        }

        has_entry_ = false;
        return *this;
    }

    DirectoryIterator operator++(int) {
        DirectoryIterator tmp(*this);
        ++(*this);
        return tmp;
    }

    bool operator==(const DirectoryIterator& other) const {
        if (has_entry_ != other.has_entry_) {
            return false;
        }
        if (!has_entry_) {
            return current_dir_.get() == other.current_dir_.get();
        }
        return current_dir_.get() == other.current_dir_.get() &&
               current_entry_.Path() == other.current_entry_.Path();
    }

    bool operator!=(const DirectoryIterator& other) const {
        return !(*this == other);
    }

private:
    std::string root_;
    std::shared_ptr<DIR> current_dir_;
    DuDirectoryEntry current_entry_;
    bool has_entry_ = false;
    bool is_root_ = false;
    uint64_t total_size_ = 0;
};

class RecursiveDirectoryIterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = DuDirectoryEntry;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    RecursiveDirectoryIterator() = default;

    RecursiveDirectoryIterator(const std::string& start_path, const Config& config)
        : config_(config) {
        struct stat st{};
        int res = 0;
        if (config.dereference) {
            res = stat(start_path.data(), &st);
        } else {
            res = lstat(start_path.data(), &st);
        }
        if (res == -1) {
            return;
        }

        if (!S_ISDIR(st.st_mode)) {
            FileID id{st.st_dev, st.st_ino};
            if (!visited_.contains(id)) {
                visited_.insert(id);
                current_value_ = value_type(start_path, st, static_cast<uint64_t>(st.st_size));
                is_valid_ = true;
                return;
            }
        }

        FileID root_id = {st.st_dev, st.st_ino};
        visited_.insert(root_id);
        DirectoryIterator root_iter(start_path);
        root_iter.SetRoot(true);
        stack_.push(root_iter);
        Process();
    }

    bool IsValid() const {
        return is_valid_;
    }

    reference operator*() const {
        return current_value_;
    }

    pointer operator->() const {
        return &current_value_;
    }

    RecursiveDirectoryIterator& operator++() {
        Process();
        return *this;
    }

    RecursiveDirectoryIterator operator++(int) {
        RecursiveDirectoryIterator tmp(*this);
        ++(*this);
        return tmp;
    }

    bool operator==(const RecursiveDirectoryIterator& other) const {
        return is_valid_ == other.is_valid_ && stack_.empty() == other.stack_.empty();
    }

    bool operator!=(const RecursiveDirectoryIterator& other) const {
        return !(*this == other);
    }

private:
    bool HandleFinishedDirectory() {
        DirectoryIterator& top = stack_.top();

        bool should_output = config_.summarize_only ? top.IsRoot() : true;

        struct stat dir_st{};
        int res = 0;

        if (config_.dereference) {
            res = stat(top.DirectoryPath().data(), &dir_st);
        } else {
            res = lstat(top.DirectoryPath().data(), &dir_st);
        }

        if (res != 0) {
            std::memset(&dir_st, 0, sizeof(dir_st));
        }

        uint64_t total_size = top.TotalSize() + static_cast<uint64_t>(dir_st.st_size);

        if (should_output) {
            current_value_ = DuDirectoryEntry(top.DirectoryPath(), dir_st, total_size);
            is_valid_ = true;
        }

        stack_.pop();

        if (!stack_.empty()) {
            stack_.top().AddToTotal(total_size);
        }

        return should_output;
    }

    bool HandleNextEntry(DirectoryIterator& top) {
        DuDirectoryEntry entry = *top;
        ++top;

        const std::string& entry_path = entry.Path();

        struct stat st{};
        if (config_.dereference) {
            if (stat(entry_path.data(), &st) == -1) {
                return false;
            }
        } else {
            st = entry.StatInfo();
        }

        FileID id{st.st_dev, st.st_ino};
        if (visited_.contains(id)) {
            return false;
        }
        visited_.insert(id);

        if (S_ISDIR(st.st_mode)) {
            DirectoryIterator child(entry_path);
            child.SetRoot(false);
            stack_.push(child);
            return false;
        }

        const uint64_t file_size = static_cast<uint64_t>(st.st_size);
        top.AddToTotal(file_size);

        if (config_.all_files) {
            current_value_ = value_type(entry_path, st, file_size);
            is_valid_ = true;
            return true;
        }

        return false;
    }

    void Process() {
        is_valid_ = false;

        while (!stack_.empty()) {
            DirectoryIterator& top = stack_.top();

            if (!top.IsValid()) {
                if (HandleFinishedDirectory()) {
                    return;
                }
                continue;
            }

            if (HandleNextEntry(top)) {
                return;
            }
        }
    }

    Config config_;
    std::stack<DirectoryIterator> stack_;
    std::set<FileID> visited_;
    value_type current_value_;
    bool is_valid_ = false;
};

class Du {
public:
    explicit Du(const Config& config) : config_(config) {
    }

    void ProcessPath(const std::string& path) const {
        RecursiveDirectoryIterator it(path, config_);
        RecursiveDirectoryIterator end;

        while (it.IsValid()) {
            const DuDirectoryEntry& entry = *it;
            std::cout << entry.Size() << "\t" << entry.Path() << std::endl;
            ++it;
        }
    }

private:
    Config config_;
};

void PrintUsageAndExit(int code) {
    std::cerr << "Usage: du [-a] [-s] [-L] PATH...\n"
              << "  -a  print sizes of all files, not only directories\n"
              << "  -s  display only a total for each argument\n"
              << "  -L  follow symbolic links\n"
              << std::endl;
    exit(code);
}

std::pair<Config, std::vector<std::string>> ParseArgs(int argc, char** argv) {
    Config config;
    std::vector<std::string> paths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg[0] != '-') {
            paths.push_back(arg);
            continue;
        }

        for (size_t j = 1; j < arg.size(); ++j) {
            switch (arg[j]) {
                case 's':
                    config.summarize_only = true;
                    break;
                case 'a':
                    config.all_files = true;
                    break;
                case 'L':
                    config.dereference = true;
                    break;
                case 'h':
                    PrintUsageAndExit(0);
                    break;
                default:
                    PrintUsageAndExit(1);
            }
        }
    }

    return {config, paths};
}

int main(int argc, char** argv) {
    auto [config, paths] = ParseArgs(argc, argv);

    if (paths.empty()) {
        paths.push_back(".");
    }

    Du du(config);

    for (const auto& path : paths) {
        du.ProcessPath(path);
    }
}
