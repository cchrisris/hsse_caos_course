#include <cstring>
#include <fcntl.h>
#include <gelf.h>
#include <iostream>
#include <libelf.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

class ElfPatcher {
public:
    enum class Mode { PrintRPath, PrintInterpreter, SetRPath, SetInterpreter };

    ElfPatcher(const std::string& file_path, Mode op_mode, const std::string& new_val = {})
        : filename(file_path), mode(op_mode), value(new_val) {
        if (elf_version(EV_CURRENT) == EV_NONE) {
            throw std::runtime_error("failed to coordinate elf library");
        }
    }

    void Process() {
        int flag = (mode == Mode::SetRPath || mode == Mode::SetInterpreter) ? O_RDWR : O_RDONLY;
        int fd = open(filename.data(), flag);

        Elf* elf = elf_begin(fd, (flag == O_RDWR) ? ELF_C_RDWR : ELF_C_READ, nullptr);

        ElfChecker(elf);

        if (mode == Mode::SetRPath || mode == Mode::SetInterpreter) {
            if (elf_flagelf(elf, ELF_C_SET, ELF_F_LAYOUT) == 0) {
                throw std::runtime_error("failed to set flags for ELF file");
            }
        }

        switch (mode) {
            case Mode::PrintRPath:
                PrintRPath(elf);
                break;
            case Mode::PrintInterpreter:
                PrintInterpreter(elf);
                break;
            case Mode::SetRPath:
                SetRPath(elf);
                if (elf_update(elf, ELF_C_WRITE) < 0) {
                    throw std::runtime_error("failed to update ELF descriptor");
                }
                break;
            case Mode::SetInterpreter:
                SetInterpreter(elf);
                if (elf_update(elf, ELF_C_WRITE) < 0) {
                    throw std::runtime_error("failed to update ELF descriptor");
                }
                break;
        }

        elf_end(elf);
        close(fd);
    }

private:
    std::string filename;
    Mode mode;
    std::string value;

    void ElfChecker(Elf* elf) {
        if (elf_kind(elf) != ELF_K_ELF) {
            throw std::runtime_error("invalid ELF format");
        }
    }

    void PrintInterpreter(Elf* elf) {
        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            throw std::runtime_error("failed to get shstr index");
        }

        Elf_Scn* scn = nullptr;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            if (!gelf_getshdr(scn, &shdr)) {
                throw std::runtime_error("failed to retrieve section header");
            }

            std::string name = elf_strptr(elf, shstrndx, shdr.sh_name);
            if (name == ".interp") {
                Elf_Data* data = elf_getdata(scn, nullptr);
                if (!data || !data->d_buf || data->d_size == 0) {
                    std::cout << "" << '\n';
                    return;
                }
                std::cout << std::string(static_cast<const char*>(data->d_buf)) << '\n';
                return;
            }
        }

        std::cout << "" << '\n';
    }

    void SetInterpreter(Elf* elf) {
        if (value.empty()) {
            throw std::runtime_error("interpreter path must be non-empty");
        }

        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            throw std::runtime_error("failed to get shstr index");
        }

        Elf_Scn* scn = nullptr;
        while ((scn = elf_nextscn(elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            if (!gelf_getshdr(scn, &shdr)) {
                throw std::runtime_error("failed to retrieve section header");
            }

            std::string name = elf_strptr(elf, shstrndx, shdr.sh_name);
            if (name == ".interp") {
                Elf_Data* data = elf_getdata(scn, nullptr);
                if (!data || !data->d_buf) {
                    throw std::runtime_error("failed to get .interp data");
                }

                size_t max_len = data->d_size;
                if (value.size() + 1 > max_len) {
                    throw std::runtime_error(
                        "new interpreter is longer than existing; resizing is not "
                        "supported");
                }
                char* buf = static_cast<char*>(data->d_buf);
                std::memset(buf, '\0', max_len);
                std::memcpy(buf, value.data(), value.size());
                elf_flagdata(data, ELF_C_SET, ELF_F_DIRTY);
                return;
            }
        }

        throw std::runtime_error(".interp section not found");
    }

    bool FindDynamicSection(Elf* elf, Elf_Scn*& dyn_scn, GElf_Shdr& dyn_shdr,
                            size_t& dynstr_index) {
        size_t shstrndx;
        if (elf_getshdrstrndx(elf, &shstrndx) < 0) {
            throw std::runtime_error("failed to retrieve shstr index");
        }

        while ((dyn_scn = elf_nextscn(elf, dyn_scn)) != nullptr) {
            if (!gelf_getshdr(dyn_scn, &dyn_shdr)) {
                throw std::runtime_error("failed to retrieve section header");
            }
            if (dyn_shdr.sh_type == SHT_DYNAMIC) {
                dynstr_index = dyn_shdr.sh_link;
                return true;
            }
        }
        return false;
    }

    void PrintRPath(Elf* elf) {
        Elf_Scn* dyn_scn = nullptr;
        GElf_Shdr dyn_shdr;
        size_t dynstr_index = 0;
        if (!FindDynamicSection(elf, dyn_scn, dyn_shdr, dynstr_index)) {
            std::cout << "" << '\n';
            return;
        }

        Elf_Data* dyn_data = elf_getdata(dyn_scn, nullptr);
        if (!dyn_data) {
            throw std::runtime_error("failed to get .dynamic data");
        }

        GElf_Dyn dyn;
        for (int i = 0; gelf_getdyn(dyn_data, i, &dyn) != nullptr; ++i) {
            if (dyn.d_tag == DT_RPATH || dyn.d_tag == DT_RUNPATH) {
                std::string path = elf_strptr(elf, dynstr_index, dyn.d_un.d_val);
                std::cout << path << '\n';
                return;
            }
        }
        std::cout << "" << '\n';
    }

    void SetRPath(Elf* elf) {
        if (value.empty()) {
            throw std::runtime_error("rpath path must be non-empty");
        }

        Elf_Scn* dyn_scn = nullptr;
        GElf_Shdr dyn_shdr;
        size_t dynstr_index = 0;
        if (!FindDynamicSection(elf, dyn_scn, dyn_shdr, dynstr_index)) {
            throw std::runtime_error(".dynamic/.dynstr not found");
        }

        Elf_Data* dyn_data = elf_getdata(dyn_scn, nullptr);
        if (!dyn_data) {
            throw std::runtime_error("failed to get .dynamic data");
        }

        int rpath_idx = -1;
        size_t rpath_off = 0;
        GElf_Dyn dyn;
        for (int i = 0; gelf_getdyn(dyn_data, i, &dyn) != nullptr; ++i) {
            if (dyn.d_tag == DT_RPATH || dyn.d_tag == DT_RUNPATH) {
                rpath_idx = i;
                rpath_off = dyn.d_un.d_val;
                break;
            }
        }
        if (rpath_idx < 0) {
            throw std::runtime_error("no rpath entry found");
        }

        Elf_Scn* dynstr_scn = elf_getscn(elf, dynstr_index);
        if (!dynstr_scn) {
            throw std::runtime_error("failed to get .dynstr section");
        }

        Elf_Data* dynstr_data = elf_getdata(dynstr_scn, nullptr);
        if (!dynstr_data || !dynstr_data->d_buf) {
            throw std::runtime_error("failed to get .dynstr data");
        }

        char* sbase = static_cast<char*>(dynstr_data->d_buf);

        size_t old_len = 0;
        for (size_t i = rpath_off; i < dynstr_data->d_size; ++i) {
            if (sbase[i] == '\0') {
                old_len = i - rpath_off;
                break;
            }
        }
        if (old_len == 0) {
            throw std::runtime_error("failed to determine old rpath length");
        }

        size_t capacity = old_len + 1;
        if (value.size() + 1 > capacity) {
            throw std::runtime_error(
                "new rpath is longer than existing; resizing is not supported");
        }

        std::memset(sbase + rpath_off, '\0', capacity);
        std::memcpy(sbase + rpath_off, value.data(), value.size());
        elf_flagdata(dynstr_data, ELF_C_SET, ELF_F_DIRTY);
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2 || argc < 3) {
        return 1;
    }

    std::string flag = argv[1];
    if (flag == "--print-rpath" && argc == 3) {
        ElfPatcher patcher(argv[2], ElfPatcher::Mode::PrintRPath);
        patcher.Process();
        return 0;
    }
    if (flag == "--print-interpreter" && argc == 3) {
        ElfPatcher patcher(argv[2], ElfPatcher::Mode::PrintInterpreter);
        patcher.Process();
        return 0;
    }
    if (flag == "--set-rpath" && argc == 4) {
        ElfPatcher patcher(argv[3], ElfPatcher::Mode::SetRPath, argv[2]);
        patcher.Process();
        return 0;
    }
    if (flag == "--set-interpreter" && argc == 4) {
        ElfPatcher patcher(argv[3], ElfPatcher::Mode::SetInterpreter, argv[2]);
        patcher.Process();
        return 0;
    }
}
