#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <numeric>
#include <limits>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <ctime>
#include <mutex>
#include <future>
#include <thread>
#include <atomic>
#include <semaphore>   // C++20, VS2022 支持
#include <Windows.h>

namespace fs = std::filesystem;

// ------------------- 全局互斥锁（用于进度输出） -------------------
std::mutex g_cerr_mutex;

// ------------------- 策略定义（表头处理） -------------------
struct KeepFirstHeaderPolicy {
    static void apply(std::ifstream& in, bool is_first_file) {
        if (!is_first_file) {
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    }
};

struct DropAllHeadersPolicy {
    static void apply(std::ifstream& in, bool /*is_first_file*/) {
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
};

struct NoHeadersPolicy {
    static void apply(std::ifstream& /*in*/, bool /*is_first_file*/) {}
};

// ------------------- 工具函数 -------------------

// 获取目录下所有 CSV 文件（含大小），按文件名排序
std::vector<std::pair<fs::path, uintmax_t>> get_csv_files(const fs::path& dir) {
    std::vector<std::pair<fs::path, uintmax_t>> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".csv") {
            // 修复 P3-9：直接用 entry.file_size() 避免重复 stat
            files.emplace_back(entry.path(), entry.file_size());
        }
    }
    std::sort(files.begin(), files.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    return files;
}

// 计算文件第一行长度（从文件开头到第一个 '\n'，包含该换行符）
// 返回 0 表示打开失败或文件无换行符（视为整个文件是表头）
uintmax_t get_first_line_length(const fs::path& file, uintmax_t file_size) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return file_size; // 打开失败，整个文件视为表头
    constexpr DWORD BUFSIZE = 4096;
    char buf[BUFSIZE];
    DWORD nRead = 0;
    uintmax_t total = 0;
    while (ReadFile(h, buf, BUFSIZE, &nRead, nullptr) && nRead > 0) {
        for (DWORD i = 0; i < nRead; ++i) {
            if (buf[i] == '\n') {
                total += (i + 1);
                CloseHandle(h);
                return total;
            }
        }
        total += nRead;
    }
    CloseHandle(h);
    // 没有找到 '\n'，整个文件视为表头
    return file_size;
}

// 生成带时间戳的文件名
fs::path generate_timestamp_filename(const fs::path& dir, const std::string& prefix = "merged") {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &in_time_t);
    std::stringstream ss;
    ss << prefix << "_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".csv";
    return dir / ss.str();
}

// ------------------- 顺序合并（用于多文件模式） -------------------
template<typename Policy>
void merge_files_sequential(const std::vector<fs::path>& files, const fs::path& output_file) {
    if (files.empty()) throw std::runtime_error("No files to merge.");

    // 预计输出总字节数（用于进度，非首文件需减去表头）
    uintmax_t total_input_size = 0;
    for (const auto& p : files) total_input_size += fs::file_size(p);

    auto space_info = fs::space(output_file.parent_path());
    if (space_info.available < total_input_size) {
        throw std::runtime_error("Insufficient disk space! Need " + std::to_string(total_input_size) +
            " bytes, but only " + std::to_string(space_info.available) + " available.");
    }

    std::ofstream out(output_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        int err = errno;
        throw std::runtime_error("Cannot create output file: " + output_file.string() +
            " (error: " + strerror(err) + ")");
    }

    // 修复 P1-2：用实际写入字节数统计进度
    uintmax_t processed_bytes = 0;
    uintmax_t total_write_bytes = 0; // 实际写入总量（预估）

    // 预先统计实际写入量（首文件全量，后续文件减表头）
    for (size_t i = 0; i < files.size(); ++i) {
        uintmax_t fsz = fs::file_size(files[i]);
        if (i == 0) {
            total_write_bytes += fsz;
        }
        else {
            uintmax_t hlen = get_first_line_length(files[i], fsz);
            total_write_bytes += (hlen < fsz) ? (fsz - hlen) : 0;
        }
    }
    if (total_write_bytes == 0) throw std::runtime_error("No data to write.");

    constexpr size_t BUF_SIZE = 1 << 20; // 1 MB 显式缓冲（修复 P3-8）
    std::vector<char> copy_buf(BUF_SIZE);

    const size_t total_files = files.size();
    for (size_t i = 0; i < total_files; ++i) {
        std::ifstream in(files[i], std::ios::binary);
        if (!in.is_open()) throw std::runtime_error("Cannot open input: " + files[i].string());

        Policy::apply(in, (i == 0));

        // 使用显式缓冲拷贝，并准确统计写入字节数（修复 P3-8, P1-2）
        while (in) {
            in.read(copy_buf.data(), BUF_SIZE);
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            out.write(copy_buf.data(), n);
            if (out.fail()) throw std::runtime_error("Write failed for: " + files[i].string());
            processed_bytes += static_cast<uintmax_t>(n);
        }
        out.flush();
        if (out.fail()) throw std::runtime_error("Flush failed.");

        {
            std::lock_guard<std::mutex> lock(g_cerr_mutex);
            int percent = static_cast<int>((processed_bytes * 100) / total_write_bytes);
            std::cerr << "\rProgress: [" << (i + 1) << "/" << total_files << "] "
                << percent << "% - " << files[i].filename().string() << "    ";
            std::cerr.flush();
        }
    }
    std::cerr << std::endl;
}

// ------------------- 多线程并行合并（single 模式，Windows API） -------------------
struct FileInfo {
    fs::path path;
    uint64_t offset;      // 在目标文件中的起始偏移（字节）
    uint64_t data_len;    // 需要复制的数据长度（字节）
    uint64_t header_len;  // 需要跳过的表头长度
};

void merge_files_parallel_win(const std::vector<fs::path>& files,
    const fs::path& output_file,
    unsigned int thread_count = 0) {
    if (files.empty()) throw std::runtime_error("No files to merge.");

    // 1. 构建每个文件的信息
    std::vector<FileInfo> infos;
    uint64_t total_size = 0;

    // 第一个文件：完整拷贝
    uint64_t first_size = fs::file_size(files[0]);
    infos.push_back({ files[0], 0, first_size, 0 });
    total_size += first_size;

    // 后续文件：跳过表头
    for (size_t i = 1; i < files.size(); ++i) {
        uint64_t file_size = fs::file_size(files[i]);
        uint64_t header_len = get_first_line_length(files[i], file_size);
        uint64_t data_len = (header_len < file_size) ? (file_size - header_len) : 0;
        if (data_len > 0) {
            infos.push_back({ files[i], total_size, data_len, header_len });
            total_size += data_len;
        }
        else {
            // 修复 P1-4：被跳过的文件打印警告
            std::lock_guard<std::mutex> lock(g_cerr_mutex);
            std::cerr << "[WARN] Skipped (no data after header): "
                << files[i].filename().string() << std::endl;
        }
    }
    if (infos.empty()) throw std::runtime_error("No data to write.");

    // 2. 预分配目标文件
    HANDLE hOut = CreateFileW(output_file.c_str(), GENERIC_WRITE,
        FILE_SHARE_WRITE | FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot create output file: " + output_file.string());
    }
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(total_size);
    if (!SetFilePointerEx(hOut, li, nullptr, FILE_BEGIN) || !SetEndOfFile(hOut)) {
        CloseHandle(hOut);
        // 修复 P2-6：创建失败立即删除
        fs::remove(output_file);
        throw std::runtime_error("Failed to set file size.");
    }
    CloseHandle(hOut);

    // 3. 确定线程数（修复 P0：实际限流）
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
    }
    thread_count = std::min(thread_count, static_cast<unsigned int>(infos.size()));
    thread_count = std::min(thread_count, 32u); // 硬上限，防止过多线程

    constexpr size_t BUFFER_SIZE = 1 << 20; // 1 MB

    // 修复 P0：用信号量限制并发线程数
    std::counting_semaphore<32> sem(thread_count);

    std::atomic<uint64_t> processed_bytes{ 0 };
    const uint64_t total_bytes = total_size;
    std::atomic<bool> has_error{ false };
    std::mutex err_mutex;
    std::string err_msg;

    std::vector<std::future<void>> futures;
    futures.reserve(infos.size());

    for (const auto& info : infos) {
        futures.push_back(std::async(std::launch::async,
            [info, &output_file, &processed_bytes, total_bytes,
            &sem, &has_error, &err_mutex, &err_msg, BUFFER_SIZE]() {

                sem.acquire(); // 限流：最多 thread_count 个线程同时运行
                struct SemGuard {
                    std::counting_semaphore<32>& s;
                    ~SemGuard() { s.release(); }
                } guard{ sem };

                if (has_error.load(std::memory_order_relaxed)) return; // 已有错误，提前退出

                // 打开源文件
                HANDLE hSrc = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN | FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hSrc == INVALID_HANDLE_VALUE) {
                    std::lock_guard<std::mutex> lk(err_mutex);
                    if (!has_error.exchange(true))
                        err_msg = "Cannot open source: " + info.path.string();
                    return;
                }

                // 跳过表头
                if (info.header_len > 0) {
                    LARGE_INTEGER liHdr;
                    liHdr.QuadPart = static_cast<LONGLONG>(info.header_len);
                    if (!SetFilePointerEx(hSrc, liHdr, nullptr, FILE_BEGIN)) {
                        CloseHandle(hSrc);
                        std::lock_guard<std::mutex> lk(err_mutex);
                        if (!has_error.exchange(true))
                            err_msg = "Seek source failed: " + info.path.string();
                        return;
                    }
                }

                // 打开目标文件（每个线程独立 HANDLE，写入区间不重叠，安全）
                HANDLE hDst = CreateFileW(output_file.c_str(), GENERIC_WRITE,
                    FILE_SHARE_WRITE | FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hDst == INVALID_HANDLE_VALUE) {
                    CloseHandle(hSrc);
                    std::lock_guard<std::mutex> lk(err_mutex);
                    if (!has_error.exchange(true))
                        err_msg = "Cannot open output for writing.";
                    return;
                }

                // 定位到目标偏移
                LARGE_INTEGER liOff;
                liOff.QuadPart = static_cast<LONGLONG>(info.offset);
                if (!SetFilePointerEx(hDst, liOff, nullptr, FILE_BEGIN)) {
                    CloseHandle(hSrc);
                    CloseHandle(hDst);
                    std::lock_guard<std::mutex> lk(err_mutex);
                    if (!has_error.exchange(true))
                        err_msg = "Seek output failed.";
                    return;
                }

                // 拷贝数据
                std::vector<char> buffer(BUFFER_SIZE);
                uint64_t remaining = info.data_len;
                bool ok = true;
                while (remaining > 0 && ok) {
                    DWORD to_read = static_cast<DWORD>(std::min<uint64_t>(BUFFER_SIZE, remaining));
                    DWORD nRead = 0;
                    if (!ReadFile(hSrc, buffer.data(), to_read, &nRead, nullptr) || nRead == 0) {
                        // 修复 P2-5：文件被截断，报错而非静默丢失
                        if (remaining > 0) {
                            std::lock_guard<std::mutex> lk(err_mutex);
                            if (!has_error.exchange(true))
                                err_msg = "Source file truncated: " + info.path.string() +
                                " (expected " + std::to_string(info.data_len) +
                                " bytes, short by " + std::to_string(remaining) + ")";
                            ok = false;
                        }
                        break;
                    }
                    DWORD written = 0;
                    if (!WriteFile(hDst, buffer.data(), nRead, &written, nullptr) || written != nRead) {
                        std::lock_guard<std::mutex> lk(err_mutex);
                        if (!has_error.exchange(true))
                            err_msg = "Write failed for: " + info.path.string();
                        ok = false;
                        break;
                    }
                    remaining -= nRead;
                    processed_bytes.fetch_add(nRead, std::memory_order_relaxed);
                }

                CloseHandle(hSrc);
                CloseHandle(hDst);
            }));
    }

    // 等待所有任务完成
    for (auto& fut : futures) {
        try { fut.get(); }
        catch (const std::exception& e) {
            // async 内部不应抛出（已内部处理），但保险起见
            throw std::runtime_error(std::string("Parallel task exception: ") + e.what());
        }
    }

    // 检查错误标志（修复 P2-6：有错误则删除输出文件）
    if (has_error.load()) {
        fs::remove(output_file);
        throw std::runtime_error("Parallel merge failed: " + err_msg);
    }

    std::cout << "\nParallel merge completed, total size: " << total_bytes << " bytes." << std::endl;
}

// ------------------- 分组算法（多文件模式） -------------------
std::vector<std::vector<fs::path>> group_files_by_size(
    const std::vector<std::pair<fs::path, uintmax_t>>& files,
    uintmax_t threshold) {

    std::vector<std::vector<fs::path>> groups;

    // 大文件：直接单独成组
    std::vector<std::pair<fs::path, uintmax_t>> small_files;
    for (const auto& item : files) {
        if (item.second >= threshold) {
            groups.push_back({ item.first });
        }
        else {
            small_files.push_back(item);
        }
    }

    // 修复 P1-3：贪心打包时，先记录索引，打包完毕后按原始顺序排列组内文件
    // small_files 目前已按文件名升序（继承自 get_csv_files 的排序）
    // 贪心选择时，记录下被选中的索引，组内按索引（即文件名）升序排列

    // 按大小降序的索引（用于贪心决策），但不修改 small_files 本身的顺序
    std::vector<size_t> size_order(small_files.size());
    std::iota(size_order.begin(), size_order.end(), 0);
    std::sort(size_order.begin(), size_order.end(),
        [&](size_t a, size_t b) { return small_files[a].second > small_files[b].second; });

    std::vector<bool> used(small_files.size(), false);
    size_t remaining_count = small_files.size();

    while (remaining_count > 0) {
        std::vector<size_t> group_indices;
        uintmax_t current_sum = 0;

        for (size_t idx : size_order) {
            if (used[idx]) continue;
            if (current_sum + small_files[idx].second <= threshold) {
                current_sum += small_files[idx].second;
                group_indices.push_back(idx);
                used[idx] = true;
                --remaining_count;
            }
        }

        if (group_indices.empty()) {
            // 兜底：取 size_order 中第一个未使用的
            for (size_t idx : size_order) {
                if (!used[idx]) {
                    group_indices.push_back(idx);
                    used[idx] = true;
                    --remaining_count;
                    break;
                }
            }
        }

        // 组内按文件名升序（即按原始索引升序）
        std::sort(group_indices.begin(), group_indices.end());
        std::vector<fs::path> group;
        group.reserve(group_indices.size());
        for (size_t idx : group_indices) {
            group.push_back(small_files[idx].first);
        }
        groups.push_back(std::move(group));
    }

    return groups;
}

// ------------------- 主函数 -------------------
int main(int argc, char* argv[]) {
    try {
        std::string mode = "multi";
        uintmax_t threshold_mb = 1024;
        std::string prefix = "merged";
        unsigned int thread_count = 0;

        if (argc < 3) {
            std::cerr << "Usage: " << argv[0]
                << " <input_dir> <output_dir> [mode] [threshold_mb] [prefix] [thread_count]\n"
                << "  mode          : 'single' or 'multi', default 'multi'\n"
                << "  threshold_mb  : positive integer (MB), default 1024\n"
                << "  prefix        : output filename prefix, default 'merged'\n"
                << "  thread_count  : parallel threads for 'single' mode (0=auto), default 0\n";
            return 1;
        }

        fs::path input_dir = argv[1];
        fs::path output_dir = argv[2];

        if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
            std::cerr << "Error: Input directory does not exist or is not a directory.\n";
            return 1;
        }
        if (!fs::exists(output_dir) || !fs::is_directory(output_dir)) {
            std::cerr << "Error: Output directory does not exist or is not a directory.\n";
            return 1;
        }
        if (fs::equivalent(input_dir, output_dir)) {
            std::cerr << "Error: Input and output directories must be different!\n";
            return 1;
        }

        if (argc >= 4) mode = argv[3];
        if (argc >= 5) {
            threshold_mb = std::stoull(argv[4]);
            if (threshold_mb == 0) { std::cerr << "Threshold must be positive.\n"; return 1; }
        }
        if (argc >= 6) prefix = argv[5];
        if (argc >= 7) thread_count = static_cast<unsigned int>(std::stoul(argv[6]));

        const uintmax_t threshold_bytes = threshold_mb * 1024ULL * 1024ULL;

        auto file_list = get_csv_files(input_dir);
        if (file_list.empty()) {
            std::cerr << "No CSV files found in input directory.\n";
            return 0;
        }

        std::cout << "Found " << file_list.size() << " CSV file(s) in " << input_dir << "\n";

        // 修复 P3-7：Policy 在此集中定义，方便后续扩展为运行时参数
        using Policy = KeepFirstHeaderPolicy;

        if (mode == "single") {
            std::vector<fs::path> all_files;
            all_files.reserve(file_list.size());
            for (const auto& p : file_list) all_files.push_back(p.first);

            fs::path output_file = generate_timestamp_filename(output_dir, prefix);
            std::cout << "Single file mode: merging all files into " << output_file << "\n";
            std::cout << "Using parallel merge with "
                << (thread_count == 0 ? "auto" : std::to_string(thread_count))
                << " threads.\n";
            merge_files_parallel_win(all_files, output_file, thread_count);
            std::cout << "Merge completed successfully!\n";
        }
        else if (mode == "multi") {
            auto groups = group_files_by_size(file_list, threshold_bytes);
            if (groups.empty()) {
                std::cerr << "No groups to process.\n";
                return 1;
            }
            std::cout << "Multi-file mode: " << groups.size() << " group(s) will be created.\n";
            for (size_t i = 0; i < groups.size(); ++i) {
                std::stringstream ss;
                ss << prefix << "_" << std::setw(3) << std::setfill('0') << (i + 1) << ".csv";
                fs::path output_file = output_dir / ss.str();
                std::cout << "Processing group " << (i + 1)
                    << " (" << groups[i].size() << " file(s)) -> " << output_file << "\n";
                merge_files_sequential<Policy>(groups[i], output_file);
            }
            std::cout << "All groups merged successfully!\n";
        }
        else {
            std::cerr << "Invalid mode: " << mode << ". Use 'single' or 'multi'.\n";
            return 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}