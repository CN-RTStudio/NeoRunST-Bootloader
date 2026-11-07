# Third-Party Notices

本仓库构建依赖 [GNU-EFI](https://github.com/ncroxon/gnu-efi) v3.0.12，其多许可结构如下（本加载器在括号内注明所用分支）：

| 组件 | 在加载器中的角色 | 上游许可 | 本项目采用 |
| --- | --- | --- | --- |
| `lib/`（`-lefi`） | EFI 运行库（Print/协议封装等） | BSD-3-Clause（Intel EFI Application Toolkit, 1998-2000 Intel / 2025 GNU-EFI Contributors） | BSD-3-Clause |
| `inc/` | EFI 规范头文件 | BSD-3-Clause（同上） | BSD-3-Clause |
| `gnuefi/`（`-lgnuefi`、`crt0-efi-x86_64.o`、`elf_x86_64_efi.lds`） | 启动代码与链接脚本 | GPL-2.0-or-later **或** BSD-3-Clause（双许可） | **BSD-3-Clause** |
| `Make.defaults` / `Make.rules` | 构建规则 | GPL-2.0-or-later 或 BSD-3-Clause（仓库顶层，双许可） | BSD-3-Clause |
| `bootloader/Makefile` 的 HP 版权头 | 构建文件（1999-2001 Hewlett-Packard） | BSD-3-Clause | BSD-3-Clause |
| `inc/subst/elf.h` | ELF 定义（BootX64.c 备用） | LGPL-2.1-or-later | 未链接分发，仅构建时头文件引用 |

由于所有被链接组件均按 BSD-3-Clause 分支使用，`BootX64.efi` 及本仓库源码不含 GPL 传染义务。

以上各上游许可的完整文本见 GNU-EFI 仓库 `licenses/` 目录（`LICENSE.efilib`、`LICENSE.BSD-3-Clause`、`LICENSE.GPL-2.0-or-later` 等）。
