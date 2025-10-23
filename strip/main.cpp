#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gelf.h>
#include <iostream>
#include <libelf.h>
#include <string>
#include <unistd.h>

class ElfStrip {
public:
    ElfStrip(const std::string& file_path, bool keep_sym = false)
        : filename(file_path), keep_symtab(keep_sym) {
        if (elf_version(EV_CURRENT) == EV_NONE) {
            throw std::runtime_error("failed to coordinate elf library");
        }
    }

    void Process() {
        int fd_original = open(filename.data(), O_RDONLY);
        Elf* og_elf = elf_begin(fd_original, ELF_C_READ, nullptr);

        ElfChecker(og_elf);

        std::string new_file_path = "temp";
        int fd_new = open(new_file_path.data(), O_RDWR | O_CREAT | O_TRUNC, 0777);
        ;
        Elf* new_elf = elf_begin(fd_new, ELF_C_WRITE, nullptr);

        ProcessElf(og_elf, new_elf);

        if (rename(new_file_path.data(), filename.data()) < 0) {
            throw std::runtime_error("failed to replace file, errno" + std::to_string(errno));
        }

        elf_end(og_elf);
        elf_end(new_elf);
        close(fd_original);
        close(fd_new);
    }

private:
    std::string filename;
    bool keep_symtab;

    void ElfChecker(Elf* elf) {
        if (elf_kind(elf) != ELF_K_ELF) {
            throw std::runtime_error("invalid ELF format");
        }
    }

    void ProcessElf(Elf* og_elf, Elf* new_elf) {
        if (elf_flagelf(new_elf, ELF_C_SET, ELF_F_LAYOUT) == 0) {
            throw std::runtime_error("failed to set flags for new_elf file");
        }

        ProcessElfHeader(og_elf, new_elf);
        ProcessProgramHeaders(og_elf, new_elf);
        ProcessSectionHeaders(og_elf, new_elf);

        if (elf_update(new_elf, ELF_C_WRITE) < 0) {
            throw std::runtime_error("failed to update ELF descriptor");
        }
    }

    void ProcessElfHeader(Elf* og_elf, Elf* new_elf) {
        GElf_Ehdr ehdr;
        if (!gelf_getehdr(og_elf, &ehdr)) {
            throw std::runtime_error("failed to retrieve object file header");
        }

        if (!gelf_newehdr(new_elf, gelf_getclass(og_elf))) {
            throw std::runtime_error("failed to create new elf header");
        }

        if (!gelf_update_ehdr(new_elf, &ehdr)) {
            throw std::runtime_error("failed to update the elf header");
        }
    }

    void ProcessProgramHeaders(Elf* og_elf, Elf* new_elf) {
        size_t phnum;
        if (elf_getphdrnum(og_elf, &phnum) < 0) {
            throw std::runtime_error("failed to get the number of program headers in the elf file");
        }

        if (!gelf_newphdr(new_elf, phnum)) {
            throw std::runtime_error("failed to create new program header");
        }

        for (size_t i = 0; i < phnum; ++i) {
            GElf_Phdr phdr;
            if (!gelf_getphdr(og_elf, i, &phdr)) {
                throw std::runtime_error("failed to retrieve the program header " +
                                         std::to_string(i));
            }

            if (!gelf_update_phdr(new_elf, i, &phdr)) {
                throw std::runtime_error("failed to update the program header " +
                                         std::to_string(i));
            }
        }
    }

    void ProcessSectionHeaders(Elf* og_elf, Elf* new_elf) {
        size_t shstrndx;
        if (elf_getshdrstrndx(og_elf, &shstrndx) < 0) {
            throw std::runtime_error("failed to get shstr index");
        }

        size_t phnum;
        if (elf_getphdrnum(og_elf, &phnum) < 0) {
            throw std::runtime_error("failed to get phnum");
        }

        size_t end_of_segments = 0;
        for (size_t i = 0; i < phnum; ++i) {
            GElf_Phdr ph;
            if (!gelf_getphdr(og_elf, i, &ph)) {
                throw std::runtime_error("failed to get phdr");
            }
            size_t end = ph.p_offset + ph.p_filesz;
            if (end > end_of_segments) {
                end_of_segments = end;
            }
        }

        Elf_Scn* scn = nullptr;
        size_t shnum = 0;
        size_t new_shstrndx = 0;
        size_t next_offset_nonalloc = end_of_segments;

        while ((scn = elf_nextscn(og_elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            if (!gelf_getshdr(scn, &shdr)) {
                throw std::runtime_error("failed to retrieve section header");
            }

            std::string name = elf_strptr(og_elf, shstrndx, shdr.sh_name);

            if ((name == ".symtab" || name == ".strtab") && !keep_symtab) {
                continue;
            }
            if (name.starts_with(".debug")) {
                continue;
            }

            if (name == ".shstrtab") {
                new_shstrndx = shnum;
            }

            Elf_Scn* new_scn = elf_newscn(new_elf);
            if (!new_scn) {
                throw std::runtime_error("elf_newscn failed");
            }

            Elf_Data* old_data = elf_getdata(scn, nullptr);
            Elf_Data* new_data = elf_newdata(new_scn);
            if (!new_data) {
                throw std::runtime_error("failed to create new data descriptor");
            }

            if (old_data && old_data->d_size) {
                new_data->d_buf = malloc(old_data->d_size);
                if (!new_data->d_buf) {
                    throw std::runtime_error("malloc failed");
                }
                if (old_data->d_buf) {
                    memcpy(new_data->d_buf, old_data->d_buf, old_data->d_size);
                } else {
                    memset(new_data->d_buf, 0, old_data->d_size);
                }
                new_data->d_size = old_data->d_size;
                new_data->d_type = old_data->d_type;
                new_data->d_align = old_data->d_align;
                new_data->d_version = old_data->d_version;
                new_data->d_off = 0;
            } else {
                new_data->d_buf = nullptr;
                new_data->d_size = 0;
                new_data->d_type = ELF_T_BYTE;
                new_data->d_align = 1;
                new_data->d_version = EV_CURRENT;
                new_data->d_off = 0;
            }

            if ((shdr.sh_flags & SHF_ALLOC) == 0) {
                size_t align = shdr.sh_addralign ? shdr.sh_addralign : 1;
                if (align > 1) {
                    if (next_offset_nonalloc % align) {
                        next_offset_nonalloc += (align - next_offset_nonalloc % align);
                    }
                }
                shdr.sh_offset = next_offset_nonalloc;
                next_offset_nonalloc += shdr.sh_size;
            }

            if (!gelf_update_shdr(new_scn, &shdr)) {
                throw std::runtime_error("gelf_update_shdr failed");
            }

            ++shnum;
        }

        GElf_Ehdr ehdr;
        if (!gelf_getehdr(og_elf, &ehdr)) {
            throw std::runtime_error("failed to get ehdr");
        }

        size_t sh_align = (gelf_getclass(og_elf) == ELFCLASS64) ? 8 : 4;
        size_t rem = next_offset_nonalloc % sh_align;
        if (rem) {
            next_offset_nonalloc += (sh_align - rem);
        }
        ehdr.e_shoff = next_offset_nonalloc;
        ehdr.e_shnum = shnum;
        ehdr.e_shstrndx = new_shstrndx;

        if (!gelf_update_ehdr(new_elf, &ehdr)) {
            throw std::runtime_error("failed to update ehdr");
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        return 1;
    }

    bool keep_symtab = false;
    std::string filename;

    if (argc == 3) {
        if (std::string(argv[1]) == "-d") {
            keep_symtab = true;
            filename = argv[2];
        } else {
            std::cerr << "unknown option " << argv[1] << '\n';
            return 1;
        }
    } else {
        filename = argv[1];
    }

    ElfStrip stripper(filename, keep_symtab);

    stripper.Process();
}
