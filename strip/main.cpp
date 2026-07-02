#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gelf.h>
#include <iostream>
#include <libelf.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

class FileDescriptorGuard {
public:
    explicit FileDescriptorGuard(int fd) : fd_(fd) {
    }

    ~FileDescriptorGuard() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    FileDescriptorGuard(const FileDescriptorGuard&) = delete;
    FileDescriptorGuard& operator=(const FileDescriptorGuard&) = delete;

    int Get() const {
        return fd_;
    }

private:
    int fd_ = -1;
};

class ElfDescriptorGuard {
public:
    explicit ElfDescriptorGuard(Elf* elf) : elf_(elf) {
    }

    ~ElfDescriptorGuard() {
        if (elf_ != nullptr) {
            elf_end(elf_);
        }
    }

    ElfDescriptorGuard(const ElfDescriptorGuard&) = delete;
    ElfDescriptorGuard& operator=(const ElfDescriptorGuard&) = delete;

    Elf* Get() const {
        return elf_;
    }

private:
    Elf* elf_ = nullptr;
};

void PrintUsageAndExit(int code) {
    std::cerr << "Usage: strip [-d] objfile...\n"
              << "  -d  remove debugging symbols only.\n"
              << std::endl;
    exit(code);
}

}  // namespace

class ElfStrip {
public:
    ElfStrip(const std::string& file_path, bool keep_sym = false)
        : filename_(file_path), keep_symtab_(keep_sym) {
        if (elf_version(EV_CURRENT) == EV_NONE) {
            throw std::runtime_error("failed to coordinate elf library");
        }
    }

    ~ElfStrip() {
        for (void* ptr : allocated_buffers_) {
            free(ptr);
        }
    }

    void Process() {
        int fd_original = open(filename_.data(), O_RDONLY);
        if (fd_original < 0) {
            throw std::runtime_error("failed to open file");
        }
        FileDescriptorGuard fd_original_guard(fd_original);

        Elf* og_elf = elf_begin(fd_original_guard.Get(), ELF_C_READ, nullptr);
        if (og_elf == nullptr) {
            throw std::runtime_error("elf_begin failed for input file");
        }
        ElfDescriptorGuard original_elf_guard(og_elf);

        ElfChecker(original_elf_guard.Get());

        std::string new_file_path = "temp";
        int fd_new = open(new_file_path.data(), O_RDWR | O_CREAT | O_TRUNC, 0777);
        if (fd_new < 0) {
            throw std::runtime_error("failed to create temp file");
        }
        FileDescriptorGuard fd_new_guard(fd_new);

        Elf* new_elf = elf_begin(fd_new_guard.Get(), ELF_C_WRITE, nullptr);
        if (new_elf == nullptr) {
            throw std::runtime_error("elf_begin failed for output file");
        }
        ElfDescriptorGuard output_elf_guard(new_elf);

        ProcessElf(original_elf_guard.Get(), output_elf_guard.Get(), keep_symtab_,
                   allocated_buffers_);

        if (rename(new_file_path.data(), filename_.data()) < 0) {
            throw std::runtime_error("failed to replace file, errno" + std::to_string(errno));
        }
    }

private:
    std::string filename_;
    bool keep_symtab_;
    std::vector<void*> allocated_buffers_;

    struct CopiedSection {
        Elf_Scn* new_scn = nullptr;
        GElf_Shdr old_shdr = {};
    };

    static size_t GetSectionCount(Elf* elf) {
        size_t shnum = 0;
        if (elf_getshdrnum(elf, &shnum) < 0) {
            throw std::runtime_error("failed to get original shnum");
        }
        return shnum;
    }

    static size_t GetProgramHeaderCount(Elf* elf) {
        size_t phnum = 0;
        if (elf_getphdrnum(elf, &phnum) < 0) {
            throw std::runtime_error("failed to get phnum");
        }
        return phnum;
    }

    static size_t ComputeEndOfSegments(Elf* elf, size_t phnum) {
        size_t end_of_segments = 0;
        for (size_t i = 0; i < phnum; ++i) {
            GElf_Phdr phdr;
            if (gelf_getphdr(elf, i, &phdr) == nullptr) {
                throw std::runtime_error("failed to get phdr");
            }

            size_t end = phdr.p_offset + phdr.p_filesz;
            if (end > end_of_segments) {
                end_of_segments = end;
            }
        }
        return end_of_segments;
    }

    static bool ShouldSkipSection(const std::string& name, bool keep_symtab) {
        if ((name == ".symtab" || name == ".strtab") && !keep_symtab) {
            return true;
        }
        if (name.starts_with(".debug")) {
            return true;
        }
        return false;
    }

    static size_t GetOldSectionIndex(Elf_Scn* scn, size_t og_shnum) {
        size_t old_section_index = elf_ndxscn(scn);
        if (old_section_index == 0 || old_section_index >= og_shnum) {
            throw std::runtime_error("failed to get old section index");
        }
        return old_section_index;
    }

    static size_t GetNewSectionIndex(Elf_Scn* scn) {
        size_t new_section_index = elf_ndxscn(scn);
        if (new_section_index == 0) {
            throw std::runtime_error("failed to get new section index");
        }
        return new_section_index;
    }

    static void CopySectionData(Elf_Scn* old_scn, Elf_Scn* new_scn,
                                std::vector<void*>& allocated_buffers) {
        Elf_Data* old_data = elf_getdata(old_scn, nullptr);
        Elf_Data* new_data = elf_newdata(new_scn);
        if (new_data == nullptr) {
            throw std::runtime_error("failed to create new data descriptor");
        }

        if (old_data != nullptr && old_data->d_size != 0) {
            new_data->d_buf = malloc(old_data->d_size);
            if (new_data->d_buf == nullptr) {
                throw std::runtime_error("malloc failed");
            }
            allocated_buffers.push_back(new_data->d_buf);

            if (old_data->d_buf != nullptr) {
                memcpy(new_data->d_buf, old_data->d_buf, old_data->d_size);
            } else {
                memset(new_data->d_buf, 0, old_data->d_size);
            }

            new_data->d_size = old_data->d_size;
            new_data->d_type = old_data->d_type;
            new_data->d_align = old_data->d_align;
            new_data->d_version = old_data->d_version;
            new_data->d_off = 0;
            return;
        }

        new_data->d_buf = nullptr;
        new_data->d_size = 0;
        new_data->d_type = ELF_T_BYTE;
        new_data->d_align = 1;
        new_data->d_version = EV_CURRENT;
        new_data->d_off = 0;
    }

    static void UpdateNonAllocSectionOffset(GElf_Shdr& shdr, size_t& next_offset_nonalloc) {
        if ((shdr.sh_flags & SHF_ALLOC) != 0) {
            return;
        }

        size_t align = shdr.sh_addralign != 0 ? shdr.sh_addralign : 1;
        if (align > 1 && (next_offset_nonalloc % align) != 0) {
            next_offset_nonalloc += (align - next_offset_nonalloc % align);
        }
        shdr.sh_offset = next_offset_nonalloc;
        next_offset_nonalloc += shdr.sh_size;
    }

    static size_t MapSectionIndex(const std::vector<size_t>& mapping, size_t old_index) {
        if (old_index == 0) {
            return 0;
        }
        if (old_index >= mapping.size()) {
            return 0;
        }
        return mapping[old_index];
    }

    static bool ShInfoIsSectionIndex(int sh_type) {
        return sh_type == SHT_REL || sh_type == SHT_RELA;
    }

    static void FixupLinks(const std::vector<size_t>& old_to_new_index,
                           std::vector<CopiedSection>& copied_sections) {
        for (CopiedSection& copied : copied_sections) {
            GElf_Shdr new_shdr;
            if (gelf_getshdr(copied.new_scn, &new_shdr) == nullptr) {
                throw std::runtime_error("failed to read new shdr");
            }

            new_shdr.sh_link = MapSectionIndex(old_to_new_index, copied.old_shdr.sh_link);

            if (ShInfoIsSectionIndex(copied.old_shdr.sh_type)) {
                new_shdr.sh_info = MapSectionIndex(old_to_new_index, copied.old_shdr.sh_info);
            }

            if (gelf_update_shdr(copied.new_scn, &new_shdr) == 0) {
                throw std::runtime_error("failed to update shdr links");
            }
        }
    }

    static void ElfChecker(Elf* elf) {
        if (elf_kind(elf) != ELF_K_ELF) {
            throw std::runtime_error("invalid ELF format");
        }
    }

    static void ProcessElf(Elf* og_elf, Elf* new_elf, bool keep_symtab,
                           std::vector<void*>& allocated_buffers) {
        if (elf_flagelf(new_elf, ELF_C_SET, ELF_F_LAYOUT) == 0) {
            throw std::runtime_error("failed to set flags for new_elf file");
        }

        ProcessElfHeader(og_elf, new_elf);
        ProcessProgramHeaders(og_elf, new_elf);
        ProcessSectionHeaders(og_elf, new_elf, keep_symtab, allocated_buffers);

        if (elf_update(new_elf, ELF_C_WRITE) < 0) {
            throw std::runtime_error("failed to update ELF descriptor");
        }
    }

    static void ProcessElfHeader(Elf* og_elf, Elf* new_elf) {
        GElf_Ehdr ehdr;
        if (gelf_getehdr(og_elf, &ehdr) == nullptr) {
            throw std::runtime_error("failed to retrieve object file header");
        }

        if (gelf_newehdr(new_elf, gelf_getclass(og_elf)) == nullptr) {
            throw std::runtime_error("failed to create new elf header");
        }

        if (gelf_update_ehdr(new_elf, &ehdr) == 0) {
            throw std::runtime_error("failed to update the elf header");
        }
    }

    static void ProcessProgramHeaders(Elf* og_elf, Elf* new_elf) {
        size_t phnum;
        if (elf_getphdrnum(og_elf, &phnum) < 0) {
            throw std::runtime_error("failed to get the number of program headers in the elf file");
        }

        if (gelf_newphdr(new_elf, phnum) == nullptr) {
            throw std::runtime_error("failed to create new program header");
        }

        for (size_t i = 0; i < phnum; ++i) {
            GElf_Phdr phdr;
            if (gelf_getphdr(og_elf, i, &phdr) == nullptr) {
                throw std::runtime_error("failed to retrieve the program header " +
                                         std::to_string(i));
            }

            if (gelf_update_phdr(new_elf, i, &phdr) == 0) {
                throw std::runtime_error("failed to update the program header " +
                                         std::to_string(i));
            }
        }
    }

    static void ProcessSectionHeaders(Elf* og_elf, Elf* new_elf, bool keep_symtab,
                                      std::vector<void*>& allocated_buffers) {
        size_t shstrndx;
        if (elf_getshdrstrndx(og_elf, &shstrndx) < 0) {
            throw std::runtime_error("failed to get shstr index");
        }

        size_t og_shnum = GetSectionCount(og_elf);
        size_t phnum = GetProgramHeaderCount(og_elf);
        size_t end_of_segments = ComputeEndOfSegments(og_elf, phnum);

        Elf_Scn* scn = nullptr;
        size_t new_shstrndx = 0;
        size_t last_new_section_index = 0;
        size_t next_offset_nonalloc = end_of_segments;
        std::vector<size_t> old_to_new_index(og_shnum, 0);
        std::vector<CopiedSection> copied_sections;

        while ((scn = elf_nextscn(og_elf, scn)) != nullptr) {
            GElf_Shdr shdr;
            if (gelf_getshdr(scn, &shdr) == nullptr) {
                throw std::runtime_error("failed to retrieve section header");
            }

            std::string name = elf_strptr(og_elf, shstrndx, shdr.sh_name);

            if (ShouldSkipSection(name, keep_symtab)) {
                continue;
            }

            size_t old_section_index = GetOldSectionIndex(scn, og_shnum);

            Elf_Scn* new_scn = elf_newscn(new_elf);
            if (new_scn == nullptr) {
                throw std::runtime_error("elf_newscn failed");
            }

            size_t new_section_index = GetNewSectionIndex(new_scn);

            if (name == ".shstrtab") {
                new_shstrndx = new_section_index;
            }

            if (new_section_index > last_new_section_index) {
                last_new_section_index = new_section_index;
            }

            old_to_new_index[old_section_index] = new_section_index;

            CopySectionData(scn, new_scn, allocated_buffers);
            UpdateNonAllocSectionOffset(shdr, next_offset_nonalloc);

            if (gelf_update_shdr(new_scn, &shdr) == 0) {
                throw std::runtime_error("gelf_update_shdr failed");
            }

            CopiedSection copied;
            copied.new_scn = new_scn;
            copied.old_shdr = shdr;
            copied_sections.push_back(copied);
        }

        FixupLinks(old_to_new_index, copied_sections);

        GElf_Ehdr ehdr;
        if (gelf_getehdr(og_elf, &ehdr) == nullptr) {
            throw std::runtime_error("failed to get ehdr");
        }

        size_t sh_align = (gelf_getclass(og_elf) == ELFCLASS64) ? 8 : 4;
        size_t rem = next_offset_nonalloc % sh_align;
        if (rem != 0) {
            next_offset_nonalloc += (sh_align - rem);
        }
        ehdr.e_shoff = next_offset_nonalloc;
        ehdr.e_shnum = last_new_section_index + 1;
        ehdr.e_shstrndx = new_shstrndx;

        if (gelf_update_ehdr(new_elf, &ehdr) == 0) {
            throw std::runtime_error("failed to update ehdr");
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        PrintUsageAndExit(1);
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
        if (std::string(argv[1]) == "-h") {
            PrintUsageAndExit(0);
        }
        filename = argv[1];
    }

    try {
        ElfStrip stripper(filename, keep_symtab);

        stripper.Process();
    } catch (const std::exception& ex) {
        std::cerr << "strip: " << ex.what() << '\n';
        return 1;
    }
}
