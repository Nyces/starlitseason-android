// 验证工具的命令层：命令行与图形界面共用同一套实现。
//
// 所有输出通过 LogSink 回调（UTF-8 文本行，不含换行），
// 命令行打印到控制台，图形界面追加到日志框。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pak.h"

namespace liveview {

using LogSink = std::function<void(const std::string&)>;

// 宽串 -> 窄串（只保留 ASCII，用于路径显示与日志）
std::string NarrowAscii(const std::wstring& text);

// UTF-8 窄串 -> 宽串（源码字面量是 UTF-8）
std::wstring WidenUtf8(const std::string& text);

// 列出目录下的 .pak（跳过 add/free/lyric/song 扩展包，与 Python 侧一致）
std::vector<std::wstring> FindPaks(const std::wstring& directory);

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data);

// 读取目录下所有 pak 的索引；每个 pak 一行日志，失败的行以 [FAIL] 开头。
// 全部失败时返回 false 并填 error。
bool LoadAllPaks(const std::wstring& directory, const std::vector<uint8_t>& key,
                 std::vector<PakInfo>* out, const LogSink& log, std::string* error);

// 输出「共 N 个 pak、条目合计 M 条」的汇总行。
void SummarizePaks(const std::vector<PakInfo>& paks, const LogSink& log);

// 按名字后缀匹配条目（忽略大小写）；名字写全名或后缀都可以。
bool FindEntryAcrossPaks(const std::vector<PakInfo>& paks, const std::string& name_or_suffix,
                         const PakInfo** pak, const PakEntry** entry);

// 列出条目，limit 限制打印行数（0 表示不限）。返回匹配数量。
int64_t ListEntries(const std::vector<PakInfo>& paks, const std::string& filter, size_t limit,
                    const LogSink& log);

// 读取并解压一个条目写到 output。
bool ExtractEntry(const std::vector<PakInfo>& paks, const std::vector<uint8_t>& key,
                  const std::string& name, const std::wstring& output, const LogSink& log,
                  std::string* error);

// 解析 .uasset/.uexp 并打印名称表规模、导出表与每条导出的属性标签。
bool DumpAsset(const std::vector<PakInfo>& paks, const std::vector<uint8_t>& key,
               const std::string& asset, const LogSink& log, std::string* error);

}  // namespace liveview
