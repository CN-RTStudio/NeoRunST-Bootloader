/*
 * NeoRunST Bootloader (improved)
 * gnu-efi 版本 >= 3.0.8
 * 编译示例:
 *   x86_64-w64-mingw32-gcc -Wall -Wextra -fno-stack-protector -fno-async-unwind-tables \
 *     -fno-pic -mno-red-zone -maccumulate-outgoing-args -mno-sse -mno-mmx -shared \
 *     -Bsymbolic -Wl,-dll -shared -e efi_main -o BootX64.efi BootX64.c -lgnuefi -loem ... 
 */
#include <efi.h>
#include <efilib.h>
#include <elf.h>

/* ============================================================
 * 数据结构(与内核 ABI 保持不变)
 * ============================================================ */
typedef struct {
    void*        BaseAddress;
    UINTN        BufferSize;
    unsigned int Width;
    unsigned int Height;
    unsigned int PixelPerScanline;
} FrameBuffer;

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

typedef struct { unsigned char magic[2]; unsigned char mode; unsigned char charSize; } PSF1_HEADER;
typedef struct { PSF1_HEADER* psf1_header; void* glyphBuffer; } PSF1_FONT;

typedef struct {
    FrameBuffer*           framebuffer;
    PSF1_FONT*             font;
    EFI_MEMORY_DESCRIPTOR* mMap;
    UINTN                  mMapSize;
    UINTN                  mMapDescriptorSize;
    void*                  rsdp;
    VOID*                  runtime_services;
} BootInfo;

/* ============================================================
 * 分级日志(unix/systemd 风格)
 * ============================================================ */
#define STAGE(t)      Print(L"\r\n── %s ──\r\n", t)
#define INFO(fmt, ...) Print(L"[  ..  ] " fmt L"\r\n", ##__VA_ARGS__)
#define OK(fmt, ...)   Print(L"[  OK  ] " fmt L"\r\n", ##__VA_ARGS__)
#define WARN(fmt, ...) Print(L"[ WARN ] " fmt L"\r\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) Print(L"[ FAIL ] " fmt L"\r\n", ##__VA_ARGS__)
#define DBG(fmt, ...)  Print(L"[ dbg  ] " fmt L"\r\n", ##__VA_ARGS__)

/* ============================================================
 * 错误捕获:打印 + 等待按键 + 重启(绝不无声卡死)
 * ============================================================ */
static VOID WaitForAnyKey(VOID)
{
    UINTN idx; EFI_INPUT_KEY k;
    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
    uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &idx);
    uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &k);
}

static VOID Fatal(CHAR16 *msg, EFI_STATUS s)
{
    Print(L"\r\n[ FAIL ] Kernel fault guard: %s\r\n", msg);
    Print(L"[ FAIL ] EFI_STATUS = 0x%llx (see efierr.h)\r\n", s);
    Print(L"\r\nPress any key to reboot...\r\n");
    WaitForAnyKey();
    uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
    for (;;) __asm__("hlt");            /* ResetSystem 竟然都失败 */
}

#define CHECK(s, what) do { EFI_STATUS _st = (s); \
        if (EFI_ERROR(_st)) Fatal(what, _st); } while (0)

static VOID FatalMsg(CHAR16 *msg) { Fatal(msg, EFI_LOAD_ERROR); }

/* ============================================================
 * 修正版内存比较(原来的 strcmp/memcp 都是错的)
 * ============================================================ */
static INTN MemCmp(const VOID* aptr, const VOID* bptr, UINTN n)
{
    const UINT8 *a = aptr, *b = bptr;
    for (UINTN i = 0; i < n; i++) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

/* ============================================================
 * 文件加载(带完整错误检查)
 * ============================================================ */
static EFI_FILE* LoadFile(EFI_FILE* Directory, CHAR16* Path,
                          EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_FILE* File = NULL;
    EFI_LOADED_IMAGE_PROTOCOL*     LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem = NULL;
    EFI_STATUS s;

    s = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
            ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(s)) { FAIL(L"Failed to get LoadedImage: 0x%llx", s); return NULL; }

    s = uefi_call_wrapper(SystemTable->BootServices->HandleProtocol, 3,
            LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&FileSystem);
    if (EFI_ERROR(s)) { FAIL(L"Failed to get filesystem: 0x%llx", s); return NULL; }

    if (Directory == NULL) {
        s = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Directory);
        if (EFI_ERROR(s)) { FAIL(L"OpenVolume failed: 0x%llx", s); return NULL; }
    }

    s = uefi_call_wrapper(Directory->Open, 5, Directory, &File, Path,
                          EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) return NULL;
    return File;
}

/* ============================================================
 * PSF1 字体加载
 * ============================================================ */
static PSF1_FONT* LoadPSF1Font(EFI_FILE* Directory, CHAR16* Path,
                               EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_FILE* FontFile = LoadFile(Directory, Path, ImageHandle, SystemTable);
    if (FontFile == NULL) { WARN(L"Font file not found: %s", Path); return NULL; }

    EFI_STATUS s;
    PSF1_HEADER* FontHeader = NULL;
    UINTN size = sizeof(PSF1_HEADER);

    s = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
            EfiLoaderData, sizeof(PSF1_HEADER), (VOID**)&FontHeader);
    CHECK(s, L"AllocatePool(font header)");

    s = uefi_call_wrapper(FontFile->Read, 3, FontFile, &size, FontHeader);
    CHECK(s, L"Read(font header)");

    if (FontHeader->magic[0] != PSF1_MAGIC0 || FontHeader->magic[1] != PSF1_MAGIC1) {
        WARN(L"PSF1 magic mismatch (got %02x %02x)", FontHeader->magic[0], FontHeader->magic[1]);
        uefi_call_wrapper(FontFile->Close, 1, FontFile);
        uefi_call_wrapper(BS->FreePool, 1, FontHeader);
        return NULL;
    }

    UINTN GlyphBufferSize = FontHeader->charSize * ((FontHeader->mode == 1) ? 512 : 256);
    UINTN GlyphBufferWant = GlyphBufferSize;
    VOID* GlyphBuffer = NULL;

    s = uefi_call_wrapper(FontFile->SetPosition, 2, FontFile, sizeof(PSF1_HEADER));
    CHECK(s, L"SetPosition(font)");
    s = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
            EfiLoaderData, GlyphBufferSize, (VOID**)&GlyphBuffer);
    CHECK(s, L"AllocatePool(glyphs)");
    s = uefi_call_wrapper(FontFile->Read, 3, FontFile, &GlyphBufferSize, GlyphBuffer);
    CHECK(s, L"Read(glyphs)");
    if (GlyphBufferSize != GlyphBufferWant) {
        WARN(L"PSF1 glyph data truncated (%d of %d bytes), font rejected",
             GlyphBufferSize, GlyphBufferWant);
        uefi_call_wrapper(FontFile->Close, 1, FontFile);
        uefi_call_wrapper(BS->FreePool, 1, GlyphBuffer);
        uefi_call_wrapper(BS->FreePool, 1, FontHeader);
        return NULL;
    }
    uefi_call_wrapper(FontFile->Close, 1, FontFile);

    PSF1_FONT* FinishedFont = NULL;
    s = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
            EfiLoaderData, sizeof(PSF1_FONT), (VOID**)&FinishedFont);
    CHECK(s, L"AllocatePool(font struct)");
    FinishedFont->psf1_header = FontHeader;
    FinishedFont->glyphBuffer = GlyphBuffer;
    return FinishedFont;
}

/* ============================================================
 * GOP 初始化(带回退:LocateProtocol → LocateHandleBuffer)
 * ============================================================ */
static FrameBuffer  GopFrame;
static FrameBuffer* InitializeGOP(VOID)
{
    EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop = NULL;
    EFI_STATUS s;

    s = uefi_call_wrapper(BS->LocateProtocol, 3, &GopGuid, NULL, (VOID**)&Gop);
    if (EFI_ERROR(s)) {
        WARN(L"LocateProtocol(GOP) failed (0x%llx), trying to enumerate handles...", s);
        UINTN HandleCount = 0; EFI_HANDLE* Handles = NULL;
        s = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                              &GopGuid, NULL, &HandleCount, &Handles);
        if (EFI_ERROR(s) || HandleCount == 0) {
            FAIL(L"No GOP display device found, firmware is text-mode only");
            return NULL;
        }
        s = uefi_call_wrapper(BS->HandleProtocol, 3,
                Handles[0], &GopGuid, (VOID**)&Gop);
        CHECK(s, L"HandleProtocol(GOP)");
    }

    GopFrame.BaseAddress       = (VOID*)Gop->Mode->FrameBufferBase;
    GopFrame.BufferSize        = Gop->Mode->FrameBufferSize;
    GopFrame.Width             = Gop->Mode->Info->HorizontalResolution;
    GopFrame.Height            = Gop->Mode->Info->VerticalResolution;
    GopFrame.PixelPerScanline  = Gop->Mode->Info->PixelsPerScanLine;
    return &GopFrame;
}

/* ============================================================
 * 内存映射获取已内联到 efi_main 尾部:
 * UEFI 规范规定第一次 ExitBootServices 之后(无论成败)固件进入受限模式,
 * 只允许 GetMemoryMap/ExitBootServices —— AllocatePool/FreePool/Print 一律违规。
 * QEMU/VB 固件宽容, VMware 严格执法直接报"固件错误"。
 * 因此在第一次 EBS 之前一次性分配带大余量的缓冲区, 重试时只复用同一缓冲区。
 * ============================================================ */

/* ============================================================
 * Unix 风格输入:可编辑 readline(方向退格 + 回显)
 * ============================================================ */
static UINTN ReadLine(CHAR16* Buf, UINTN Max)
{
    UINTN Len = 0;
    if (Max == 0) return 0;
    for (;;) {
        UINTN idx;
        uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &idx);
        EFI_INPUT_KEY Key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);

        if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
            Print(L"\r\n"); Buf[Len] = 0; return Len;
        }
        if (Key.UnicodeChar == 0x08 || Key.UnicodeChar == 0x7F) {   /* backspace */
            if (Len) { Len--; Print(L"\b \b"); }
            continue;
        }
        if (Key.UnicodeChar == 0x03) {                              /* Ctrl+C */
            Print(L"^C\r\n"); Buf[0] = 0; return 0;
        }
        if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar < 0x7F && Len < Max - 1) {
            Buf[Len++] = Key.UnicodeChar;
            Print(L"%c", Key.UnicodeChar);
        }
    }
}

/* ============================================================
 * 内存映射打印(memmap 命令用)
 * ============================================================ */
static const CHAR16* MemTypeStr(UINT32 t)
{
    switch (t) {
        case EfiReservedMemoryType:      return L"reserved";
        case EfiLoaderCode:              return L"loader-code";
        case EfiLoaderData:              return L"loader-data";
        case EfiBootServicesCode:        return L"bs-code";
        case EfiBootServicesData:        return L"bs-data";
        case EfiRuntimeServicesCode:     return L"rt-code";
        case EfiRuntimeServicesData:     return L"rt-data";
        case EfiConventionalMemory:      return L"free";
        case EfiUnusableMemory:          return L"unusable";
        case EfiACPIReclaimMemory:       return L"acpi-reclaim";
        case EfiACPIMemoryNVS:           return L"acpi-nvs";
        case EfiMemoryMappedIO:          return L"mmio";
        case EfiMemoryMappedIOPortSpace: return L"mmio-port";
        case EfiPalCode:                 return L"pal";
        case EfiPersistentMemory:        return L"persistent";
        default:                         return L"unknown";
    }
}

static VOID PrintMemoryMap(EFI_MEMORY_DESCRIPTOR* Map, UINTN MapSize, UINTN DescSize)
{
    Print(L"  %-16s %-14s %-10s %s\r\n", L"TYPE", L"START", L"PAGES", L"BYTES");
    if (DescSize == 0) return;
    for (UINTN off = 0; off < MapSize; off += DescSize) {
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)((CHAR8*)Map + off);
        Print(L"  %-16ls 0x%012lx %-10lx %ld KB\r\n",
              MemTypeStr(d->Type), d->PhysicalStart, d->NumberOfPages,
              (d->NumberOfPages * 4096) / 1024);
    }
}

/* ============================================================
 * Shell(unix 风格提示符)
 * ============================================================ */
static VOID Shell(FrameBuffer* Fb, PSF1_FONT* Font, VOID* Rsdp,
                  Elf64_Ehdr* Hdr)
{
    CHAR16 Buf[128];
    Print(L"\r\nType 'help' for available commands.\r\n");
    for (;;) {
        Print(L"neorunstboot> ");
        ReadLine(Buf, 128);
        if (!Buf[0]) continue;

        if (!StrCmp(Buf, L"help")) {
            Print(L"  help     - show this help\r\n"
                  L"  boot     - boot the kernel\r\n"
                  L"  info     - show hardware/load info\r\n"
                  L"  memmap   - show EFI memory map\r\n"
                  L"  clear    - clear screen\r\n"
                  L"  reboot   - reboot the machine\r\n");
        }
        else if (!StrCmp(Buf, L"boot")) return;
        else if (!StrCmp(Buf, L"info")) {
            Print(L"  framebuffer : %d x %d, bpp-scanline %d, base 0x%lx, size 0x%lx\r\n",
                  Fb ? Fb->Width : 0, Fb ? Fb->Height : 0,
                  Fb ? Fb->PixelPerScanline : 0,
                  Fb ? (UINT64)(UINTN)Fb->BaseAddress : 0,
                  Fb ? Fb->BufferSize : 0);
            Print(L"  font        : %s, charSize = %d\r\n",
                  Font ? L"PSF1 loaded" : L"<none>",
                  Font ? Font->psf1_header->charSize : 0);
            Print(L"  ACPI RSDP   : %s (0x%lx)\r\n",
                  Rsdp ? L"found" : L"NOT FOUND", (UINT64)(UINTN)Rsdp);
            Print(L"  kernel      : entry 0x%lx, phnum %d, type %s\r\n",
                  (UINT64)Hdr->e_entry, Hdr->e_phnum,
                  Hdr->e_type == ET_EXEC ? L"EXEC" : L"DYN(PIE)");
        }
        else if (!StrCmp(Buf, L"memmap")) {
            UINTN sz = 0, key, dsz; UINT32 dv;
            EFI_MEMORY_DESCRIPTOR* tmp = NULL;
            /* 即取即打,拿最新的一份 */
            uefi_call_wrapper(BS->GetMemoryMap, 5, &sz, NULL, &key, &dsz, &dv);
            sz += 2 * dsz;
            if (!EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3,
                    EfiLoaderData, sz, (VOID**)&tmp)))
                if (!EFI_ERROR(uefi_call_wrapper(BS->GetMemoryMap, 5,
                        &sz, tmp, &key, &dsz, &dv)))
                    PrintMemoryMap(tmp, sz, dsz);
            uefi_call_wrapper(BS->FreePool, 1, tmp);
        }
        else if (!StrCmp(Buf, L"clear")) {
            uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
        }
        else if (!StrCmp(Buf, L"reboot")) {
            uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
        }
        else {
            Print(L"neorunboot: command not found: %ls (try 'help')\r\n", Buf);
        }
    }
}

/* ============================================================
 * bootdebug 检测 + 内核崩溃标志(NVRAM)
 * ============================================================
 * - 根目录存在 "bootdebug" 文件 → 开机直接进 Shell
 * - 内核 panic 时会通过 Runtime Services 写入 NVRAM 标志
 *   "NeoRunSTCrash" 并复位, 下次启动检测到该标志自动进 Shell
 */
static EFI_GUID gCrashVarGuid = {
    0x4e6f5275, 0x6e53, 0x5443,
    {0xA1, 0xB2, 0x3C, 0x4D, 0x5E, 0x6F, 0x70, 0x81}
};

static BOOLEAN CheckBootDebug(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    EFI_FILE* f = LoadFile(NULL, L"bootdebug", ImageHandle, SystemTable);
    if (f != NULL) {
        uefi_call_wrapper(f->Close, 1, f);
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN CheckCrashFlag(VOID)
{
    /* size=0/Data=NULL: 变量存在 → EFI_BUFFER_TOO_SMALL, 不存在 → EFI_NOT_FOUND */
    UINTN size = 0;
    EFI_STATUS s = uefi_call_wrapper(RT->GetVariable, 5,
            L"NeoRunSTCrash", &gCrashVarGuid, NULL, &size, NULL);
    return (BOOLEAN)(s == EFI_BUFFER_TOO_SMALL || s == EFI_SUCCESS);
}

static VOID ClearCrashFlag(VOID)
{
    uefi_call_wrapper(RT->SetVariable, 5,
            L"NeoRunSTCrash", &gCrashVarGuid, 0, 0, NULL);
}

/* ============================================================
 * main
 * ============================================================ */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    /* 关掉固件看门狗,防止加载慢被复位 */
    uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    Print(L"NeoRunST Bootloader by CustomOS\r\n");
    Print(L"EFI %d.%02d | Firmware: %s\r\n",
          (SystemTable->Hdr.Revision >> 16), (SystemTable->Hdr.Revision & 0xFFFF),
          SystemTable->FirmwareVendor);

    /* ---------- bootdebug / 内核崩溃标志检测 ---------- */
    BOOLEAN DebugMode = CheckBootDebug(ImageHandle, SystemTable);
    BOOLEAN Crashed   = CheckCrashFlag();
    if (DebugMode) OK(L"bootdebug found in root dir -> entering debug shell");
    if (Crashed) {
        WARN(L"Kernel crash flag detected (NeoRunSTCrash) -> last boot panicked!");
        ClearCrashFlag();
        Print(L"[ WARN ] This boot enters the debug shell, type 'boot' to try again\r\n");
    }

    /* ---------- 阶段 1: 加载内核文件 ---------- */
    STAGE(L"Stage 1/5  Load kernel file");
    EFI_FILE* Kernel = LoadFile(NULL, L"runst\\system\\runstknl.nep",
                                ImageHandle, SystemTable);
    if (Kernel == NULL) FatalMsg(L"runst\\system\\runstknl.nep not found");
    OK(L"Kernel file opened");

    Elf64_Ehdr Header;
    UINTN KernelFileSize = 0;
    {
        UINTN FileInfoSize = 0;
        EFI_FILE_INFO* FileInfo = NULL;
        /* 两段式查询: 第一次传 Size=0/Buffer=NULL, 固件按规范返回 EFI_BUFFER_TOO_SMALL
           并把所需大小写回 FileInfoSize —— 这是预期结果, 只有其他错误才致命 */
        {
            EFI_STATUS gs = Kernel->GetInfo(Kernel, &gEfiFileInfoGuid, &FileInfoSize, NULL);
            if (EFI_ERROR(gs) && gs != EFI_BUFFER_TOO_SMALL) Fatal(L"GetInfo(size)", gs);
        }
        CHECK(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
              FileInfoSize, (VOID**)&FileInfo), L"AllocatePool(fileinfo)");
        CHECK(Kernel->GetInfo(Kernel, &gEfiFileInfoGuid, &FileInfoSize, (VOID**)&FileInfo),
              L"GetInfo");

        UINTN size = sizeof(Header);
        CHECK(Kernel->Read(Kernel, &size, &Header), L"Read(ELF header)");
        KernelFileSize = FileInfo->FileSize;
        OK(L"File size: %ld bytes", (UINT64)FileInfo->FileSize);
    }

    if (MemCmp(&Header.e_ident[EI_MAG0], ELFMAG, SELFMAG) != 0 ||
        Header.e_ident[EI_CLASS] != ELFCLASS64 ||
        Header.e_ident[EI_DATA]  != ELFDATA2LSB ||
        Header.e_machine != EM_X86_64 || Header.e_version != EV_CURRENT ||
        (Header.e_type != ET_EXEC && Header.e_type != ET_DYN)) {
        FatalMsg(L"Invalid kernel ELF format (need 64-bit LSB x86-64 EXEC/DYN)");
    }
    OK(L"ELF header verified (entry = 0x%lx, %s)", (UINT64)Header.e_entry,
       Header.e_type == ET_EXEC ? L"EXEC" : L"DYN");

    /* ---------- 阶段 2: 按 program header 装载段 ---------- */
    STAGE(L"Stage 2/5  Load kernel segments");
    if (Header.e_phentsize != sizeof(Elf64_Phdr) || Header.e_phnum == 0) {
        FatalMsg(L"Invalid program header table (bad e_phentsize/e_phnum)");
    }
    Elf64_Phdr* Phdrs = NULL;
    CHECK(Kernel->SetPosition(Kernel, Header.e_phoff), L"SetPosition(phdrs)");
    {
        UINTN want = Header.e_phnum * Header.e_phentsize;
        UINTN size = want;
        CHECK(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
              size, (VOID**)&Phdrs), L"AllocatePool(phdrs)");
        CHECK(Kernel->Read(Kernel, &size, Phdrs), L"Read(phdrs)");
        if (size != want) FatalMsg(L"Program header table truncated");
    }

    int LoadedSegs = 0;
    UINTN KernelEntryPhys = 0;   /* 物理入口: e_entry 所在段的加载地址 + 段内偏移 */
    for (Elf64_Phdr* Phdr = Phdrs;
         (CHAR8*)Phdr < (CHAR8*)Phdrs + Header.e_phnum * Header.e_phentsize;
         Phdr = (Elf64_Phdr*)((CHAR8*)Phdr + Header.e_phentsize))
    {
        if (Phdr->p_type != PT_LOAD) continue;

        if (Phdr->p_memsz == 0 || Phdr->p_filesz > Phdr->p_memsz) {
            FatalMsg(L"Invalid PT_LOAD segment size (filesz > memsz or memsz == 0)");
        }
        if (Phdr->p_offset > KernelFileSize ||
            Phdr->p_filesz > KernelFileSize - Phdr->p_offset) {
            FatalMsg(L"PT_LOAD segment exceeds kernel file boundary");
        }

        UINTN Pages = (Phdr->p_memsz + 0xFFF) / 0x1000;
        UINT64 Segment = Phdr->p_paddr ? Phdr->p_paddr : Phdr->p_vaddr;

        EFI_STATUS s = uefi_call_wrapper(BS->AllocatePages, 5, AllocateAddress,
                EfiLoaderData, Pages, &Segment);
        if (EFI_ERROR(s)) {
            FAIL(L"Cannot allocate %d pages at 0x%lx (0x%llx), trying fallback...", Pages, Segment, s);
            Segment = 0x100000;   /* 回退: 1MiB 以上任意可用位置 */
            CHECK(uefi_call_wrapper(BS->AllocatePages, 5, AllocateAnyPages,
                  EfiLoaderData, Pages, &Segment), L"AllocatePages(fallback failed)");
            WARN(L"Segment relocated to 0x%lx, make sure the kernel is position-independent!", Segment);
        }

        CHECK(Kernel->SetPosition(Kernel, Phdr->p_offset), L"SetPosition(segment)");
        UINTN size = Phdr->p_filesz;
        CHECK(Kernel->Read(Kernel, &size, (VOID*)(UINTN)Segment), L"Read(segment)");
        if (size != Phdr->p_filesz) FatalMsg(L"PT_LOAD segment truncated (short read)");

        /* BSS: memsz > filesz 的部分清零 */
        if (Phdr->p_memsz > Phdr->p_filesz)
            uefi_call_wrapper(BS->SetMem, 3,
                (VOID*)(UINTN)(Segment + Phdr->p_filesz),
                Phdr->p_memsz - Phdr->p_filesz, 0);

        INFO(L"Segment: vaddr 0x%012lx -> 0x%012lx, %d pages", (UINT64)Phdr->p_vaddr,
             Segment, Pages);

        /* 高半区内核: e_entry 是链接地址(高半区), 物理入口 = 段加载地址 +
           (e_entry - 段 vaddr)。低半区 stub 段内 vaddr == paddr, 同样适用 */
        if (Header.e_entry >= Phdr->p_vaddr &&
            Header.e_entry <  Phdr->p_vaddr + Phdr->p_memsz) {
            KernelEntryPhys = Segment + (Header.e_entry - Phdr->p_vaddr);
        }
        LoadedSegs++;
    }
    if (LoadedSegs == 0) FatalMsg(L"Kernel has no PT_LOAD segments");
    if (KernelEntryPhys == 0) FatalMsg(L"Kernel e_entry lies outside every PT_LOAD segment");
    OK(L"%d segments loaded", LoadedSegs);
    uefi_call_wrapper(Kernel->Close, 1, Kernel);

    /* ---------- 阶段 3: 字体 ---------- */
    STAGE(L"Stage 3/5  Load font");
    PSF1_FONT* Font = LoadPSF1Font(NULL, L"runst\\font\\zap-light16.psf",
                                   ImageHandle, SystemTable);
    if (Font == NULL) { WARN(L"No font, kernel will be unable to draw text (continuing)"); }
    else OK(L"PSF1 font ready, charSize = %d", Font->psf1_header->charSize);

    /* ---------- 阶段 4: 显卡 ---------- */
    STAGE(L"Stage 4/5  Init display (GOP)");
    FrameBuffer* Fb = InitializeGOP();
    if (Fb == NULL) { WARN(L"No GOP framebuffer, kernel will be text-only (continuing)"); }
    else {
        OK(L"Resolution %d x %d, scanline %d", Fb->Width, Fb->Height, Fb->PixelPerScanline);
        DBG(L"fb: base 0x%lx, size 0x%lx", (UINT64)(UINTN)Fb->BaseAddress, Fb->BufferSize);
    }

    /* ---------- 阶段 5: ACPI ---------- */
    STAGE(L"Stage 5/5  Find ACPI RSDP");
    VOID* Rsdp = NULL;
    {
        static EFI_GUID Acpi2Guid = ACPI_20_TABLE_GUID;
        static EFI_GUID Acpi1Guid = ACPI_TABLE_GUID;
        EFI_CONFIGURATION_TABLE* Ct = SystemTable->ConfigurationTable;
        for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
            if (CompareGuid(&Ct[i].VendorGuid, &Acpi2Guid) &&
                !MemCmp(Ct[i].VendorTable, "RSD PTR ", 8)) {
                Rsdp = Ct[i].VendorTable; OK(L"ACPI 2.0+ RSDP found @ 0x%lx", (UINT64)(UINTN)Rsdp);
                break;
            }
            if (Rsdp == NULL && CompareGuid(&Ct[i].VendorGuid, &Acpi1Guid) &&
                !MemCmp(Ct[i].VendorTable, "RSD PTR ", 8)) {
                Rsdp = Ct[i].VendorTable;   /* 老机器回退 ACPI 1.0, 继续找 2.0 */
                WARN(L"Only ACPI 1.0 RSDP found @ 0x%lx (legacy platform?)", (UINT64)(UINTN)Rsdp);
            }
        }
        if (Rsdp == NULL) WARN(L"RSDP not found, kernel will have no power/ACPI management");
    }

    /* ---------- Shell (仅 bootdebug 或崩溃后进入) ---------- */
    if (DebugMode || Crashed) {
        Shell(Fb, Font, Rsdp, &Header);
    }

    /* ---------- 获取内存映射 → ExitBootServices ----------
       UEFI 规范: 第一次调用 ExitBootServices 之后(无论成败)固件进入受限模式,
       只允许 GetMemoryMap/ExitBootServices —— AllocatePool/FreePool/Print 一律违规,
       VMware 固件对违规直接报"固件错误"。
       所以在第一次 EBS 之前就分配好带 128 个描述符余量的缓冲区,
       之后即使固件偷偷分配内存使 map 变大, 重试也只写回同一缓冲区, 零分配零打印。 */
    STAGE(L"Hand over to kernel");
    EFI_MEMORY_DESCRIPTOR* Map = NULL;
    UINTN MapSize = 0, MapKey = 0, DescSize = 0; UINT32 DescVer = 0;
    UINTN BufCap = 0;   /* 缓冲区总容量, 保持不变(GetMemoryMap 会覆盖传入的 size) */
    {
        /* 先探一次大小 */
        UINTN sz = 0, dummyKey;
        EFI_STATUS gs = uefi_call_wrapper(BS->GetMemoryMap, 5,
                &sz, NULL, &dummyKey, &DescSize, &DescVer);
        if (gs != EFI_BUFFER_TOO_SMALL) CHECK(gs, L"GetMemoryMap(size)");

        BufCap = sz + 128 * (DescSize ? DescSize : 48);
        CHECK(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
              BufCap, (VOID**)&Map), L"AllocatePool(map)");

        MapSize = BufCap;
        CHECK(uefi_call_wrapper(BS->GetMemoryMap, 5,
              &MapSize, Map, &MapKey, &DescSize, &DescVer), L"GetMemoryMap");
        /* 之后到 ExitBootServices 之间不允许任何其他调用(包括 Print) */
    }

    /* ---------- 填 BootInfo ---------- */
    BootInfo BI;
    BI.framebuffer        = Fb;
    BI.font               = Font;
    BI.mMap               = Map;
    BI.mMapSize           = MapSize;
    BI.mMapDescriptorSize = DescSize;
    BI.rsdp               = Rsdp;
    BI.runtime_services   = (VOID*)SystemTable->RuntimeServices;

    /* ---------- ExitBootServices(合规重试) ----------
       第一次失败后(MapKey 被固件内部分配作废, VMware 上比 QEMU 更易发生):
       受限模式下只复用同一缓冲区调 GetMemoryMap + ExitBootServices,
       绝不 AllocatePool/FreePool/Print —— 违规调用正是之前 VMware
       报"firmware error"的直接原因。全部失败则静默硬复位。 */
    EFI_STATUS s = EFI_SUCCESS;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) {
            UINTN cap = BufCap;   /* 每次重试都报完整的缓冲区容量 */
            s = uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
                                  &cap, Map, &MapKey, &DescSize, &DescVer);
            if (EFI_ERROR(s)) break;   /* 连余量都不够 → 无解, 走静默复位 */
            MapSize = cap;
            BI.mMap = Map;
            BI.mMapSize = MapSize;
            BI.mMapDescriptorSize = DescSize;
        }
        s = uefi_call_wrapper(SystemTable->BootServices->ExitBootServices,
                              2, ImageHandle, MapKey);
        if (!EFI_ERROR(s)) break;
    }
    if (EFI_ERROR(s)) {   /* 此时 Boot Services 已不可靠, 只能静默硬复位 */
        uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
        for (;;) __asm__("hlt");
    }

    /* 调试探针: 跳内核前把屏幕涂成品红色。
       品红出现后死 => 崩在内核接管之后; 屏幕没变品红 => 崩在固件侧 */
    if (Fb != NULL && Fb->BaseAddress != NULL) {
        volatile UINT32* fbpx = (volatile UINT32*)Fb->BaseAddress;
        UINTN n = Fb->BufferSize / 4;
        for (UINTN i = 0; i < n; i++) fbpx[i] = 0x00FF00FF;   /* magenta */
    }

    /* 注意: ExitBootServices 成功后 Boot Services(含 ConOut)即告失效,
       此后绝不能再 Print(否则解引用已释放的控制台驱动内存 → #GP),
       直接跳转内核。入口地址已在 Stage 1 的日志里输出过 */
    void (*KernelStart)(BootInfo*) =
        ((__attribute__((sysv_abi)) void (*)(BootInfo*)) KernelEntryPhys);
    KernelStart(&BI);

    /* 内核正常情况下不该返回 */
    Print(L"[ FAIL ] Kernel returned!\r\n");
    for (;;) __asm__("hlt");
    return EFI_LOAD_ERROR;
}
