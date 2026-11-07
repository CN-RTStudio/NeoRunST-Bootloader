# NeoRunST Bootloader

NeoRunST CustomOS 的 UEFI 引导加载器，基于 [GNU-EFI](https://github.com/ncroxon/gnu-efi)（v3.0.12+）构建。

## 功能

- **五阶段加载流程**：内核文件校验 → 段装载 → PSF1 字体 → GOP 显示 → ACPI RSDP，全程分级日志（`OK / WARN / FAIL / dbg`）
- **ELF64 内核加载**：支持 `ET_EXEC` / `ET_DYN`（PIE），按 program header 逐段装载并清零 BSS；自动把高半区链接地址（`0xFFFF8000…`）换算为物理入口
- **Fail-guard**：每个 EFI 调用带错误检查，失败打印 `EFI_STATUS` 并等待按键后复位，绝不无声卡死
- **调试 Shell**：磁盘根目录存在 `bootdebug` 文件时进入交互 shell（`help / boot / info / memmap / clear / reboot`）
- **崩溃记录**：内核 panic 时经 Runtime Services 写入 NVRAM 标志 `NeoRunSTCrash` 并复位；下次启动检测到该标志自动进入调试 shell
- **合规的 ExitBootServices 重试**：首次 EBS 前一次性分配带余量的内存映射缓冲区，重试时零分配零打印——解决 VMware 固件对受限模式违规调用报 "firmware error" 的问题
- **硬件探测容错**：字体 / GOP / RSDP 缺失时降级继续（内核以无字体、文本模式等形态运行），不直接失败

## 构建

本源码需要在 GNU-EFI 构建树内编译（依赖其 `lib/`、`gnuefi/` 产物与构建规则）：

```bash
# 1. 获取并编译 GNU-EFI (Ubuntu / Debian)
sudo apt-get install -y build-essential git
git clone https://github.com/ncroxon/gnu-efi.git
cd gnu-efi && make -C lib && make -C gnuefi

# 2. 把本仓库 src/ 下的 BootX64.c 与 Makefile 放入 gnu-efi 的 bootloader/ 目录
cp /path/to/NeoRunST-Bootloader/src/BootX64.c  bootloader/
cp /path/to/NeoRunST-Bootloader/src/Makefile   bootloader/

# 3. 构建
make -C bootloader
```

产物：`bootloader/BootX64.efi`。部署到 EFI 分区时命名为 `runstload.efi`（配合 GRUB chainloader）或 `EFI/BOOT/BootX64.efi`（直接引导）。

若较新版本 gcc 触发 `-Werror`，在 `src/Makefile` 的 CFLAGS 追加对应的 `-Wno-error=xxx`（当前已带 `-Wno-error=unused-parameter -Wno-error=unused-variable`）。

## 内核启动协议（BootInfo ABI）

内核以 System V AMD64 调用约定接收唯一的 `BootInfo*` 参数（物理地址）：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `framebuffer` | `FrameBuffer*` | GOP 帧缓冲（基址/尺寸/宽高/扫描线像素数） |
| `font` | `PSF1_FONT*` | PSF1 字体（头 + 字形缓冲），可能为 NULL |
| `mMap` | `EFI_MEMORY_DESCRIPTOR*` | UEFI 内存映射 |
| `mMapSize` | `UINTN` | 映射总字节数 |
| `mMapDescriptorSize` | `UINTN` | 单描述符字节数 |
| `rsdp` | `void*` | ACPI 2.0+ RSDP（老平台回退 1.0），可能为 NULL |
| `runtime_services` | `VOID*` | UEFI Runtime Services 表（EBS 后仍可用） |

镜像布局约定：内核 ELF 位于 `runst/system/runstknl.nep`，字体位于 `runst/font/*.psf`。

## 许可证

本项目源码以 **BSD-3-Clause** 许可发布（见 `LICENSE`）。

所用上游组件（见 `THIRD-PARTY-NOTICES.md`）：GNU-EFI v3.0.12 为多许可发布——本加载器链接的 `lib/`（`-lefi`）、`inc/` 头文件为 BSD-3-Clause（Intel EFI Application Toolkit 条款），`gnuefi/`（`-lgnuefi`、crt0、链接脚本）为 GPL-2.0-or-later **或** BSD-3-Clause 双许可，本项目选择 BSD-3-Clause 分支，因此引导器二进制不含 GPL 义务。

**范围声明**：本 bootloader 仅将 NeoRunST 内核（`runstknl.nep`，ELF64 文件）作为普通数据文件从磁盘读入内存并跳转——与 GRUB 加载 Linux 内核的关系相同，二者是独立的程序，不构成一个合并作品。**NeoRunST 内核与操作系统的其余部分不在本仓库、不受本许可覆盖**（内核闭源专有，详见主项目声明）。
