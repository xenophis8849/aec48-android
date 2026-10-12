# 第三方声明

## pffft

`third_party/pffft/`，静态链接进 `libaec48.so`。来源：<https://github.com/marton78/pffft>。

本库对 pffft 的修改：`src/pffft_priv_impl.h` 的 setup 创建函数检查两处内存分配，失败时返回 NULL（原实现直接解引用空指针）。

**分发包含 `libaec48.so` 的软件（例如 App）时，须在随附文档或其他材料中附上以下版权声明、条款与免责声明**（pffft 许可对二进制分发的要求）。

```

Copyright (c) 2020  Dario Mambro ( dario.mambro@gmail.com )
Copyright (c) 2019  Hayati Ayguen ( h_ayguen@web.de )
Copyright (c) 2013  Julien Pommier ( pommier@modartt.com )

Copyright (c) 2004 the University Corporation for Atmospheric
Research ("UCAR"). All rights reserved. Developed by NCAR's
Computational and Information Systems Laboratory, UCAR,
www.cisl.ucar.edu.

Redistribution and use of the Software in source and binary forms,
with or without modification, is permitted provided that the
following conditions are met:

- Neither the names of NCAR's Computational and Information Systems
Laboratory, the University Corporation for Atmospheric Research,
nor the names of its sponsors or contributors may be used to
endorse or promote products derived from this Software without
specific prior written permission.  

- Redistributions of source code must retain the above copyright
notices, this list of conditions, and the disclaimer below.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions, and the disclaimer below in the
documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
SOFTWARE.

```

## KleidiAI

`third_party/kleidiai/`（节选，未修改，见其 `README.md`），静态链接进 `libaec48.so`。来源：<https://gitlab.arm.com/kleidi/kleidiai>（v1.32.0）。许可：Apache License 2.0，全文见 `third_party/kleidiai/LICENSES/Apache-2.0.txt`。

**分发包含 `libaec48.so` 的软件（例如 App）时，须附上 Apache License 2.0 全文及以下版权声明**（Apache-2.0 第 4 条）。KleidiAI 未附 NOTICE 文件。

```
SPDX-FileCopyrightText: Copyright 2024-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
SPDX-License-Identifier: Apache-2.0
```

## 测试向量

`test/vec/` 下的录音取自 Microsoft ICASSP 2023 Acoustic Echo Cancellation Challenge 盲测集（远端单讲），来源：<https://github.com/microsoft/AEC-Challenge>。按 Microsoft 提供该数据集时的条款使用（"as is"，见上述仓库 README 的 Dataset licenses 一节），仅用于本库自检，不随库分发。`*_expected*.pcm` 为本库处理上述录音的输出。
