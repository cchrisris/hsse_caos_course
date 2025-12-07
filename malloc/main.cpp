#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <unistd.h>

#include <sys/mman.h>

namespace {

namespace constants {

constexpr size_t kMmapThreshold = 131'072;
constexpr size_t kBrkSize = kMmapThreshold * 8;
constexpr size_t kMaxSize = 1'073'741'824;  // 1GB

constexpr size_t kMinChunkSize = 32;
constexpr size_t kAlignment = 16;
constexpr size_t kAlignmentMask = kAlignment - 1;

constexpr size_t kFastMax = 104;
constexpr size_t kFastConsolidate = 65'536;
constexpr size_t kFastBinsCount = 10;

constexpr size_t kBinsCount = 126;
constexpr size_t kMaxSmallBinSize = 1024;
constexpr size_t kSmallBinStep = 16;
constexpr size_t kSmallBinBase = 2;
constexpr size_t kLargeBinStart = 62;
constexpr double kBigBinBase = 1.125;

constexpr int kMetaSize = sizeof(size_t);
constexpr size_t kMetaMask = static_cast<size_t>(0b111);
constexpr size_t kSizeMask = ~kMetaMask;
constexpr size_t kMmapMask = static_cast<size_t>(0b10);
constexpr size_t kPrevInUseMask = static_cast<size_t>(0b1);

constexpr int kMmapHeaderOffset = 8;

}  // namespace constants

namespace byte {

template <typename T>
T* Advance(T* ptr, int offset) {
    using ByteType = std::conditional_t<std::is_const_v<T>, const std::byte, std::byte>;
    return reinterpret_cast<T*>(reinterpret_cast<ByteType*>(ptr) + offset);
}

template <typename T, typename U>
size_t Difference(T* first, U* second) {
    return reinterpret_cast<std::byte*>(first) - reinterpret_cast<std::byte*>(second);
}

}  // namespace byte

size_t GetAlignedSize(size_t size) {
    size_t total = (size + constants::kMetaSize + constants::kAlignmentMask) /
                   constants::kAlignment * constants::kAlignment;
    return total < constants::kMinChunkSize ? constants::kMinChunkSize : total;
}

size_t GetBinIndex(size_t size) {
    if (size <= constants::kMaxSmallBinSize) {
        size_t index = size / constants::kSmallBinStep;
        return index >= constants::kSmallBinBase ? index - constants::kSmallBinBase : 0;
    }

    double normalized =
        static_cast<double>(size) / static_cast<double>(constants::kMaxSmallBinSize);
    double log_val = std::log(normalized) / std::log(constants::kBigBinBase);
    size_t index = constants::kLargeBinStart + static_cast<size_t>(log_val);
    return index < constants::kBinsCount ? index : constants::kBinsCount - 1;
}

struct Node;

class Metadata {
public:
    bool IsValid() const {
        return full_meta_ > 0 && full_meta_ <= (constants::kMaxSize | constants::kMetaMask);
    }

    size_t Size() const {
        return full_meta_ & constants::kSizeMask;
    }

    size_t Flags() const {
        return full_meta_ & constants::kMetaMask;
    }

    bool IsMmaped() const {
        return (full_meta_ & constants::kMmapMask) != 0;
    }

    bool IsPrevInUse() const {
        return (full_meta_ & constants::kPrevInUseMask) != 0;
    }

    bool IsInUse() const {
        return Next().IsPrevInUse();
    }

    Metadata& Next() {
        return *byte::Advance(this, Size());
    }

    const Metadata& Next() const {
        return *byte::Advance(this, Size());
    }

    Metadata& Prev() {
        size_t prev_back_meta =
            *reinterpret_cast<size_t*>(byte::Advance(this, -constants::kMetaSize));
        return *byte::Advance(this, -static_cast<int>(prev_back_meta & constants::kSizeMask));
    }

    Node* AsNode() {
        return reinterpret_cast<Node*>(this);
    }

    void* RawPtr() {
        if (IsMmaped()) {
            return byte::Advance(this, -constants::kMmapHeaderOffset);
        }
        return this;
    }

    void* UserPtr() {
        return byte::Advance(this, constants::kMetaSize);
    }

    void MarkFree() {
        Next().full_meta_ &= ~constants::kPrevInUseMask;
        WriteBackSize();
    }

    void* UsePtr() {
        if (!IsMmaped()) {
            Next().full_meta_ |= constants::kPrevInUseMask;
        }
        return UserPtr();
    }

    void SetSize(size_t new_size) {
        full_meta_ = new_size | Flags();
    }

    static Metadata& Create(void* ptr, size_t value) {
        auto& meta = *reinterpret_cast<Metadata*>(ptr);
        meta.full_meta_ = value;
        return meta;
    }

    static Metadata& FromUserPtr(void* ptr) {
        return *byte::Advance(reinterpret_cast<Metadata*>(ptr), -constants::kMetaSize);
    }

private:
    void WriteBackSize() {
        *reinterpret_cast<size_t*>(byte::Advance(this, Size() - constants::kMetaSize)) = full_meta_;
    }

    size_t full_meta_;
};

struct Node {
    Metadata meta;
    Node* next = nullptr;
    Node* prev = nullptr;
};

template <size_t BinCount>
class BinsList {
public:
    void Add(Metadata& meta) {
        Node* node = meta.AsNode();
        size_t index = GetBinIndex(meta.Size());

        node->next = bins_[index];
        node->prev = nullptr;
        if (bins_[index] != nullptr) {
            bins_[index]->prev = node;
        }
        bins_[index] = node;
    }

    Metadata* FindAndRemove(size_t min_size) {
        size_t index = GetBinIndex(min_size);
        while (index < BinCount) {
            if (bins_[index] != nullptr) {
                auto node = bins_[index];
                while (node != nullptr) {
                    if (node->meta.Size() == min_size || (node->meta.Size() >= min_size + constants::kMinChunkSize)) {
                        Remove(node);
                        return &node->meta;
                    }
                    node = node->next;
                }
            }
            ++index;
        }
        return nullptr;
    }

    void Remove(Node* node) {
        size_t index = GetBinIndex(node->meta.Size());
        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            bins_[index] = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        }
    }

    Node* ExtractAll() {
        Node* result = nullptr;
        for (auto*& head : bins_) {
            while (head != nullptr) {
                Node* current = head;
                head = head->next;
                current->next = result;
                result = current;
            }
        }
        return result;
    }

private:
    Node* bins_[BinCount] = {};
};

class Heap {
public:
    void* Allocate(size_t size) {
        if (!Initialize() || !Resize(size)) {
            return nullptr;
        }

        Metadata& meta = Metadata::Create(first_, size | constants::kPrevInUseMask);
        first_ = byte::Advance(first_, size);
        Metadata::Create(first_, 0);
        return meta.UsePtr();
    }

    void ReturnToTop(Metadata& meta) {
        meta.MarkFree();
        first_ = meta.RawPtr();
    }

    bool IsAtBegin(void* ptr) const {
        return ptr == begin_;
    }

    bool IsAtTop(void* ptr) const {
        return ptr == first_;
    }

private:
    size_t AvailableSpace() const {
        return byte::Difference(last_, first_);
    }

    bool Initialize() {
        if (first_ != nullptr) {
            return true;
        }

        void* base = sbrk(constants::kBrkSize);
        if (base == reinterpret_cast<void*>(-1)) {
            return false;
        }

        size_t misalignment =
            (reinterpret_cast<uintptr_t>(base) + constants::kMetaSize) % constants::kAlignment;
        size_t offset = misalignment == 0 ? 0 : constants::kAlignment - misalignment;

        begin_ = byte::Advance(base, offset);
        first_ = begin_;
        last_ = byte::Advance(base, constants::kBrkSize);
        return true;
    }

    bool Resize(size_t needed) {
        if (AvailableSpace() < needed + constants::kMetaSize) {
            if (sbrk(constants::kBrkSize) == reinterpret_cast<void*>(-1)) {
                return false;
            }
            last_ = byte::Advance(last_, constants::kBrkSize);
        }
        return true;
    }

    void* begin_ = nullptr;
    void* first_ = nullptr;
    void* last_ = nullptr;
};

BinsList<constants::kFastBinsCount> fast_bins;
BinsList<constants::kBinsCount> bins;
Heap heap;

void* MmapAllocate(size_t size) {
    size_t total_size = size + constants::kMmapHeaderOffset;
    void* region =
        mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        return nullptr;
    }
    Metadata& meta = Metadata::Create(byte::Advance(region, constants::kMmapHeaderOffset),
                                      total_size | constants::kMmapMask);
    return meta.UsePtr();
}

void* MmapReallocate(void* ptr, size_t new_size) {
    Metadata& meta = Metadata::FromUserPtr(ptr);
    size_t new_total = new_size + constants::kMmapHeaderOffset;

    void* new_region = mremap(meta.RawPtr(), meta.Size(), new_total, MREMAP_MAYMOVE);
    if (new_region == MAP_FAILED) {
        return nullptr;
    }

    Metadata& new_meta = Metadata::Create(byte::Advance(new_region, constants::kMmapHeaderOffset),
                                          new_total | constants::kMmapMask);
    return new_meta.UsePtr();
}

void AddToBins(Metadata& meta) {
    meta.MarkFree();
    bins.Add(meta);
}

Metadata* MergeWithPrev(Metadata* meta) {
    if (heap.IsAtBegin(meta->RawPtr()) || meta->IsPrevInUse()) {
        return meta;
    }

    Metadata& prev = meta->Prev();
    if (!prev.IsValid()) {
        return meta;
    }

    bins.Remove(prev.AsNode());
    prev.SetSize(prev.Size() + meta->Size());
    return &prev;
}

Metadata* MergeWithNext(Metadata* meta) {
    if (heap.IsAtTop(meta->Next().RawPtr())) {
        return meta;
    }

    Metadata& next = meta->Next();
    if (next.IsInUse()) {
        return meta;
    }

    bins.Remove(next.AsNode());
    meta->SetSize(meta->Size() + next.Size());
    return meta;
}

Metadata* MergeWithNeighbors(Metadata* meta) {
    meta = MergeWithPrev(meta);
    meta = MergeWithNext(meta);
    return meta;
}

void FreeChunk(Metadata& meta) {
    Metadata* merged = MergeWithNeighbors(&meta);

    if (heap.IsAtTop(merged->Next().RawPtr())) {
        heap.ReturnToTop(*merged);
    } else {
        AddToBins(*merged);
    }
}

void SplitIfPossible(Metadata& meta, size_t requested_size) {
    size_t available = meta.Size();
    size_t remaining = available - requested_size;

    if (remaining >= constants::kMinChunkSize) {
        meta.SetSize(requested_size);
        Metadata& split = Metadata::Create(byte::Advance(meta.RawPtr(), requested_size),
                                           remaining | constants::kPrevInUseMask);
        AddToBins(split);
    }
}

void* AllocateFromFastBins(size_t size) {
    Metadata* meta = fast_bins.FindAndRemove(size);
    return (meta != nullptr) ? meta->UsePtr() : nullptr;
}

void* AllocateFromBins(size_t size) {
    Metadata* meta = bins.FindAndRemove(size);
    if (meta == nullptr) {
        return nullptr;
    }
    SplitIfPossible(*meta, size);
    return meta->UsePtr();
}

void ConsolidateFastBins() {
    for (Node* node = fast_bins.ExtractAll(); node != nullptr;) {
        Node* next = node->next;
        FreeChunk(node->meta);
        node = next;
    }
}

void ReportError(const char* message) {
    write(STDERR_FILENO, message, strlen(message));
    abort();
}

}  // namespace

void* malloc(size_t size) {
    size_t chunk_size = GetAlignedSize(size);

    if (chunk_size > constants::kMmapThreshold) {
        return MmapAllocate(chunk_size);
    }

    void* ptr = nullptr;
    if (chunk_size <= constants::kFastMax) {
        ptr = AllocateFromFastBins(chunk_size);
    } else {
        ConsolidateFastBins();
    }

    if (ptr == nullptr) {
        ptr = AllocateFromBins(chunk_size);
    }
    if (ptr == nullptr) {
        ptr = heap.Allocate(chunk_size);
    }
    return ptr;
}

void* calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* ptr = malloc(total);
    if (ptr != nullptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t new_size) {
    if (ptr == nullptr) {
        return malloc(new_size);
    }

    Metadata& meta = Metadata::FromUserPtr(ptr);
    if (!meta.IsValid()) {
        ReportError("invalid pointer passed to realloc\n");
    }

    size_t new_chunk_size = GetAlignedSize(new_size);
    size_t old_size = meta.Size();

    if (meta.IsMmaped()) {
        return MmapReallocate(ptr, new_chunk_size);
    }

    if (new_chunk_size <= old_size) {
        return ptr;
    }

    void* new_ptr = malloc(new_size);
    if (new_ptr != nullptr) {
        size_t copy_size = std::min(old_size - constants::kMetaSize, new_size);
        memcpy(new_ptr, ptr, copy_size);
        free(ptr);
    }
    return new_ptr;
}

void free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    Metadata& meta = Metadata::FromUserPtr(ptr);
    if (!meta.IsValid()) {
        ReportError("invalid pointer passed to free\n");
    }
    if (!meta.IsMmaped() && !meta.IsInUse()) {
        ReportError("double free detected\n");
    }

    if (meta.IsMmaped()) {
        munmap(meta.RawPtr(), meta.Size());
        return;
    }

    size_t size = meta.Size();
    if (size >= constants::kFastConsolidate) {
        ConsolidateFastBins();
    } else if (size <= constants::kFastMax) {
        fast_bins.Add(meta);
        return;
    }

    FreeChunk(meta);
}