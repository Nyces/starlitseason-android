"""StarLit Season 资源读取工具包。

当前阶段只实现验证所需的 UE4.24 pak v8 子集：
- 索引解析（AES-256-ECB 加密索引）
- 条目提取（Zlib 分块解压、条目级 AES）
- uasset 包头解析（用于资产普查）
"""

__version__ = "0.1.0"
