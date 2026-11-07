// liveview —— live 演出验证程序
//
// 里程碑 1（已完成）：
//   双击启动 → 选游戏资源目录 → 输入 AES 密钥（记住到本机配置）→
//   原生解析 pak 索引并校验（AES-256-ECB / UE4 pak v8）。
// 里程碑 2（进行中）：
//   条目解压（zlib）→ UE4 cooked 包解析（名称表 / 导入导出 / 属性标签）。
// 后续：骨骼网格与动画解析、Vulkan 渲染舞台与角色、音轨同步。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>

#include "aes256.h"
#include "bundle.h"
#include "config.h"
#include "dialogs.h"
#include "pak.h"
#include "ue4package.h"

#ifdef LIVEVIEW_RENDER
#include "render_vulkan.h"
#endif

namespace {

void PrintUsage() {
    std::printf(
        "用法:\n"
        "  liveview                                   双击模式：选目录、输密钥、校验 pak\n"
        "  liveview --launcher                        同上\n"
        "  liveview --selftest                        自检（AES 测试向量 + 密钥解析）\n"
        "  liveview --paks <目录> --key <密钥> [命令]  命令行操作\n"
        "      （默认）校验索引；--list [关键字] 列条目；\n"
        "      --extract <条目名> <输出文件> 导出条目；--dump <资产路径> 打印包结构\n"
        "  liveview <bundle>                          打印包信息与首末关键帧\n"
        "  liveview <bundle> --at <帧>                输出该帧插值后的机位\n"
        "  liveview <bundle> --play <秒>              按 tick 频率逐帧推进并打印机位\n"
        "  liveview <bundle> --render[=秒]            打开窗口按时间轴播放（需渲染后端）\n");
}

void PrintPose(const liveview::Bundle& bundle, double frame) {
    const liveview::CameraKey pose = liveview::SampleCamera(bundle, frame);
    std::printf("帧 %lld  机位 %.2f %.2f %.2f  瞄准 %.2f %.2f %.2f\n",
                static_cast<long long>(frame), pose.position[0], pose.position[1],
                pose.position[2], pose.target[0], pose.target[1], pose.target[2]);
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

// UTF-8 窄串 -> UTF-16（给 MessageBox 显示中文用）
std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length);
    return out;
}

std::string Lower(const std::string& text) {
    std::string out = text;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::vector<std::wstring> FindPaks(const std::wstring& directory) {
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW((directory + L"\\*.pak").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return paths;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::wstring name = data.cFileName;
        std::wstring lower = name;
        for (wchar_t& ch : lower) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        // 与 Python 侧一致：默认跳过 add/free/lyric/song 这几类扩展包
        if (lower.rfind(L"add", 0) == 0 || lower.rfind(L"free", 0) == 0 ||
            lower.rfind(L"lyric", 0) == 0 || lower.rfind(L"song", 0) == 0) {
            continue;
        }
        paths.push_back(directory + L"\\" + name);
    } while (FindNextFileW(handle, &data) != 0);
    FindClose(handle);
    return paths;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written,
                              nullptr) != 0 &&
                    written == data.size();
    CloseHandle(handle);
    return ok;
}

bool LoadAllPaks(const std::wstring& directory, const std::vector<uint8_t>& key,
                 std::vector<liveview::PakInfo>* out, std::string* error, int* failed) {
    const std::vector<std::wstring> paths = FindPaks(directory);
    if (paths.empty()) {
        *error = "目录下没有找到 .pak 文件";
        return false;
    }
    int failures = 0;
    for (const std::wstring& path : paths) {
        liveview::PakInfo info;
        std::string pak_error;
        if (liveview::LoadPakIndex(path, key, &info, &pak_error)) {
            out->push_back(std::move(info));
        } else {
            std::printf("  [FAIL] %s: %s\n", Narrow(path).c_str(), pak_error.c_str());
            ++failures;
        }
    }
    if (failed != nullptr) {
        *failed = failures;
    }
    if (out->empty()) {
        *error = "所有 pak 索引都解析失败（密钥可能不对）";
        return false;
    }
    return true;
}

// 校验目录下所有 pak 的索引；summary 给消息框显示。
bool CheckPaks(const std::wstring& directory, const std::vector<uint8_t>& key,
               std::wstring* summary, std::string* error, int* ok_paks = nullptr,
               int64_t* entries_total = nullptr) {
    const std::vector<std::wstring> paths = FindPaks(directory);
    if (paths.empty()) {
        *error = "目录下没有找到 .pak 文件";
        return false;
    }
    std::wstring text;
    int64_t total = 0;
    int ok_count = 0;
    for (const std::wstring& path : paths) {
        liveview::PakInfo info;
        std::string pak_error;
        if (!liveview::LoadPakIndex(path, key, &info, &pak_error)) {
            std::printf("  [FAIL] %s: %s\n", Narrow(path).c_str(), pak_error.c_str());
            text += L"  [FAIL] " + path.substr(directory.size() + 1) + L": " + Widen(pak_error) +
                    L"\n";
            continue;
        }
        ++ok_count;
        total += static_cast<int64_t>(info.entries.size());
        std::printf("  %s  version=%d entries=%lld mount=%s\n", Narrow(path).c_str(),
                    static_cast<int>(info.version), static_cast<long long>(info.entries.size()),
                    info.mount_point.c_str());
        text += L"  " + path.substr(directory.size() + 1) + L"  v" +
                std::to_wstring(info.version) + L"  条目 " + std::to_wstring(info.entries.size()) +
                L"\n";
    }
    *summary = L"游戏目录：" + directory + L"\n\n" + text + L"\n共 " + std::to_wstring(ok_count) +
               L" / " + std::to_wstring(paths.size()) + L" 个 pak 索引解析成功，条目合计 " +
               std::to_wstring(total) + L" 条。";
    if (ok_paks != nullptr) {
        *ok_paks = ok_count;
    }
    if (entries_total != nullptr) {
        *entries_total = total;
    }
    return ok_count > 0;
}

// 在所有 pak 里找一个条目（按后缀匹配，忽略大小写和扩展名即传入完整后缀）。
bool FindEntryAcrossPaks(const std::vector<liveview::PakInfo>& paks, const std::string& suffix,
                         const liveview::PakInfo** pak_out, const liveview::PakEntry** entry_out) {
    const std::string lower = Lower(suffix);
    for (const liveview::PakInfo& pak : paks) {
        for (const liveview::PakEntry& entry : pak.entries) {
            const std::string name = Lower(entry.name);
            if (name.size() >= lower.size() &&
                name.compare(name.size() - lower.size(), lower.size(), lower) == 0) {
                *pak_out = &pak;
                *entry_out = &entry;
                return true;
            }
        }
    }
    return false;
}

int CommandList(const std::vector<liveview::PakInfo>& paks, const std::string& filter) {
    const std::string needle = Lower(filter);
    int64_t total = 0;
    int matched = 0;
    for (const liveview::PakInfo& pak : paks) {
        for (const liveview::PakEntry& entry : pak.entries) {
            ++total;
            if (!needle.empty() && Lower(entry.name).find(needle) == std::string::npos) {
                continue;
            }
            ++matched;
            if (matched <= 200) {
                std::printf("%-110s usize=%-9lld method=%d\n", entry.name.c_str(),
                            static_cast<long long>(entry.usize), entry.method);
            }
        }
    }
    std::printf("\n共 %lld 条，匹配 %d 条\n", static_cast<long long>(total), matched);
    return 0;
}

int CommandExtract(const std::vector<liveview::PakInfo>& paks, const std::vector<uint8_t>& key,
                   const std::string& name, const std::wstring& output) {
    const liveview::PakInfo* pak = nullptr;
    const liveview::PakEntry* entry = nullptr;
    if (!FindEntryAcrossPaks(paks, name, &pak, &entry)) {
        std::fprintf(stderr, "找不到条目: %s\n", name.c_str());
        return 1;
    }
    std::vector<uint8_t> data;
    std::string error;
    if (!liveview::ReadEntry(*pak, *entry, key, &data, &error)) {
        std::fprintf(stderr, "读取失败: %s\n", error.c_str());
        return 1;
    }
    if (!WriteFileBytes(output, data)) {
        std::fprintf(stderr, "写入失败\n");
        return 1;
    }
    std::printf("%s -> %s  (%zu 字节, usize=%lld)\n", entry->name.c_str(), Narrow(output).c_str(),
                data.size(), static_cast<long long>(entry->usize));
    return 0;
}

int CommandDump(const std::vector<liveview::PakInfo>& paks, const std::vector<uint8_t>& key,
                const std::string& asset) {
    // 资产路径形如 Model/Character/.../S_chr_body_base；先拿 .uasset 再找同名 .uexp
    std::string stem = asset;
    if (Lower(stem).size() > 7 && Lower(stem).compare(Lower(stem).size() - 7, 7, ".uasset") == 0) {
        stem = stem.substr(0, stem.size() - 7);
    }
    const liveview::PakInfo* uasset_pak = nullptr;
    const liveview::PakEntry* uasset_entry = nullptr;
    if (!FindEntryAcrossPaks(paks, stem + ".uasset", &uasset_pak, &uasset_entry)) {
        std::fprintf(stderr, "找不到资产: %s.uasset\n", stem.c_str());
        return 1;
    }

    std::string error;
    std::vector<uint8_t> uasset;
    if (!liveview::ReadEntry(*uasset_pak, *uasset_entry, key, &uasset, &error)) {
        std::fprintf(stderr, "读取 uasset 失败: %s\n", error.c_str());
        return 1;
    }

    std::vector<uint8_t> uexp;
    const liveview::PakInfo* uexp_pak = nullptr;
    const liveview::PakEntry* uexp_entry = nullptr;
    if (FindEntryAcrossPaks(paks, stem + ".uexp", &uexp_pak, &uexp_entry)) {
        if (!liveview::ReadEntry(*uexp_pak, *uexp_entry, key, &uexp, &error)) {
            std::fprintf(stderr, "读取 uexp 失败: %s\n", error.c_str());
            return 1;
        }
    }

    liveview::PackageData package;
    if (!liveview::ParsePackage(std::move(uasset), std::move(uexp), &package, &error)) {
        std::fprintf(stderr, "解析包失败: %s\n", error.c_str());
        return 1;
    }

    std::printf("包 %s\n", package.package_name.c_str());
    std::printf("  header=%d  名称=%zu  导入=%zu  导出=%zu  uexp=%zu 字节\n", package.header_size,
                package.names.size(), package.imports.size(), package.exports.size(),
                package.uexp.size());
    std::printf("  条目 %s\n", uasset_entry->name.c_str());

    for (const liveview::ExportObject& object : package.exports) {
        std::printf("\n[导出] %s  (%s)  序列 %lld 字节 @ %lld\n", object.name.c_str(),
                    object.class_name.c_str(), static_cast<long long>(object.serial_size),
                    static_cast<long long>(object.serial_offset));
        std::vector<uint8_t> bytes;
        std::string export_error;
        if (!liveview::ExportBytes(package, object, &bytes, &export_error)) {
            std::printf("    <定位失败: %s>\n", export_error.c_str());
            continue;
        }
        const std::vector<liveview::PropertyTag> props =
            liveview::ParseProperties(bytes, package.names, 0);
        if (props.empty()) {
            std::printf("    <没有解析到属性>\n");
            continue;
        }
        for (const liveview::PropertyTag& prop : props) {
            std::printf("    %-28s %-16s size=%-8d", prop.name.c_str(), prop.type.c_str(),
                        prop.size);
            if (!prop.struct_name.empty()) {
                std::printf(" struct=%s", prop.struct_name.c_str());
            }
            if (!prop.inner_type.empty()) {
                std::printf(" inner=%s", prop.inner_type.c_str());
            }
            std::printf("\n");
        }
    }
    return 0;
}

int RunLauncher() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    liveview::Config config;
    liveview::LoadConfig(&config);
    std::string error;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (config.game_dir.empty()) {
            std::wstring folder;
            if (!liveview::PickGameFolder(&folder)) {
                CoUninitialize();
                return 1;
            }
            config.game_dir = folder;
        }
        std::vector<uint8_t> key;
        if (!liveview::ParseHexKey(config.key_hex, &key)) {
            std::string entered;
            if (!liveview::AskAesKey(&entered, config.key_hex)) {
                CoUninitialize();
                return 1;
            }
            config.key_hex = entered;
            if (!liveview::ParseHexKey(config.key_hex, &key)) {
                continue;
            }
        }

        std::wstring summary;
        if (CheckPaks(config.game_dir, key, &summary, &error)) {
            liveview::SaveConfig(config);
            MessageBoxW(nullptr, summary.c_str(), L"liveview —— pak 校验通过",
                        MB_OK | MB_ICONINFORMATION);
            CoUninitialize();
            return 0;
        }
        MessageBoxW(nullptr, (L"校验失败：\n" + Widen(error) + L"\n\n请重新选择目录或密钥。").c_str(),
                    L"liveview", MB_OK | MB_ICONWARNING);
        config.game_dir.clear();
        config.key_hex.clear();
    }
    CoUninitialize();
    return 1;
}

// 自检：AES-256 按 FIPS-197 测试向量核对，密钥解析做正反例。
// CI 上没有游戏数据，用它保证密码学部分没有回归。
int RunSelfTest() {
    const std::string hex = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    std::vector<uint8_t> key;
    if (!liveview::ParseHexKey(hex, &key) || key.size() != 32) {
        std::fprintf(stderr, "自检失败: 密钥解析\n");
        return 1;
    }
    const uint8_t cipher[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
                                0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
    const uint8_t expected[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t plain[16];
    {
        uint8_t block[16];
        std::memcpy(block, cipher, 16);
        uint8_t round_keys[240];
        liveview::Aes256ExpandKey(key.data(), round_keys);
        liveview::Aes256DecryptBlock(round_keys, block, plain);
    }
    if (std::memcmp(plain, expected, 16) != 0) {
        std::fprintf(stderr, "自检失败: AES-256 测试向量\n");
        return 1;
    }
    // 反例：长度不对、含非十六进制字符都必须被拒
    std::vector<uint8_t> bad;
    if (liveview::ParseHexKey("1234", &bad) || liveview::ParseHexKey(std::string(64, 'z'), &bad)) {
        std::fprintf(stderr, "自检失败: 非法密钥未被拒绝\n");
        return 1;
    }
    std::printf("自检通过：AES-256 测试向量 + 密钥解析\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return RunLauncher();
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        PrintUsage();
        return 0;
    }
    if (std::strcmp(argv[1], "--launcher") == 0) {
        return RunLauncher();
    }
    if (std::strcmp(argv[1], "--selftest") == 0) {
        return RunSelfTest();
    }

    if (std::strcmp(argv[1], "--paks") == 0) {
        if (argc < 3) {
            PrintUsage();
            return 1;
        }
        std::wstring directory;
        {
            const std::string narrow = argv[2];
            directory.assign(narrow.begin(), narrow.end());
        }
        std::string key_hex;
        std::string command = "check";
        std::vector<std::string> rest;
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--key" && i + 1 < argc) {
                key_hex = argv[++i];
            } else if (option == "--list") {
                command = "list";
            } else if (option == "--extract") {
                command = "extract";
            } else if (option == "--dump") {
                command = "dump";
            } else {
                rest.push_back(option);
            }
        }

        std::vector<uint8_t> key;
        if (!liveview::ParseHexKey(key_hex, &key)) {
            std::fprintf(stderr, "密钥必须是 64 位十六进制\n");
            return 1;
        }

        std::vector<liveview::PakInfo> paks;
        std::string error;
        int failed = 0;
        if (!LoadAllPaks(directory, key, &paks, &error, &failed)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }

        if (command == "list") {
            return CommandList(paks, rest.empty() ? std::string() : rest[0]);
        }
        if (command == "extract") {
            if (rest.size() < 2) {
                PrintUsage();
                return 1;
            }
            std::wstring output(rest[1].begin(), rest[1].end());
            return CommandExtract(paks, key, rest[0], output);
        }
        if (command == "dump") {
            if (rest.empty()) {
                PrintUsage();
                return 1;
            }
            return CommandDump(paks, key, rest[0]);
        }

        int ok_paks = 0;
        int64_t entries_total = 0;
        std::wstring summary;
        if (!CheckPaks(directory, key, &summary, &error, &ok_paks, &entries_total)) {
            std::fprintf(stderr, "pak check failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("\n%d pak(s) OK, %lld entries total\n", ok_paks,
                    static_cast<long long>(entries_total));
        return 0;
    }

    const std::string path = argv[1];
    liveview::Bundle bundle;
    std::string error;
    if (!liveview::LoadBundle(path, &bundle, &error)) {
        std::fprintf(stderr, "读取失败: %s\n", error.c_str());
        return 1;
    }

    std::printf("包信息\n");
    std::printf("  资源      %s\n", bundle.asset.c_str());
    std::printf("  相机绑定  %s\n", bundle.binding.c_str());
    std::printf("  tick      %.3f fps\n", bundle.tick_fps);
    std::printf("  范围      %lld .. %lld（步长 %lld）\n",
                static_cast<long long>(bundle.range_start),
                static_cast<long long>(bundle.range_end),
                static_cast<long long>(bundle.range_step));
    std::printf("  关键帧    %zu 个\n", bundle.keys.size());

    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--at" && i + 1 < argc) {
            PrintPose(bundle, std::atof(argv[++i]));
        } else if (option == "--play" && i + 1 < argc) {
            const double seconds = std::atof(argv[++i]);
            if (bundle.tick_fps <= 0.0) {
                std::fprintf(stderr, "包内 tick 非法，无法播放\n");
                return 1;
            }
            const int64_t total = static_cast<int64_t>(seconds * bundle.tick_fps);
            const int64_t display_step = static_cast<int64_t>(bundle.tick_fps) / 60;
            if (display_step <= 0) {
                std::fprintf(stderr, "tick 太低，无法按 60 帧输出\n");
                return 1;
            }
            for (int64_t frame = 0; frame <= total; frame += display_step) {
                PrintPose(bundle, static_cast<double>(frame));
            }
        } else if (option == "--render" || option.rfind("--render=", 0) == 0) {
#ifdef LIVEVIEW_RENDER
            double seconds = -1.0;
            if (option.size() > 9) {
                seconds = std::atof(option.c_str() + 9);
            } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                seconds = std::atof(argv[++i]);
            }
            return liveview::RunRender(bundle, seconds);
#else
            std::fprintf(stderr, "本构建未启用渲染后端（需用 -DLIVEVIEW_RENDER=ON 重新构建）\n");
            return 1;
#endif
        } else {
            PrintUsage();
            return 1;
        }
    }
    return 0;
}
