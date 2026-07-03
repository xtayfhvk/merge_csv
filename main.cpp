#define _CRT_SECURE_NO_WARNINGS
// merge_csv.cpp
// C++17, 编译: g++ -std=c++17 -pthread main.cpp -lstdc++fs -o merge_csv
// 或 VS2022 直接编译

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

namespace fs = std::filesystem;

// ------------------- 全局互斥锁 -------------------
std::mutex g_cerr_mutex;

// ------------------- 策略定义 -------------------
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

// ------------------- 获取文件列表（带大小） -------------------
std::vector<std::pair<fs::path, uintmax_t>> get_csv_files(const fs::path& dir) {
    std::vector<std::pair<fs::path, uintmax_t>> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".csv") {
            files.emplace_back(entry.path(), fs::file_size(entry.path()));
        }
    }
    std::sort(files.begin(), files.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    return files;
}

// ------------------- 合并指定文件列表（模板） -------------------
template<typename Policy>
void merge_files(const std::vector<fs::path>& files, const fs::path& output_file) {
    if (files.empty()) {
        throw std::runtime_error("No files to merge.");
    }

    uintmax_t total_size = 0;
    for (const auto& p : files) {
        total_size += fs::file_size(p);
    }

    auto space_info = fs::space(output_file.parent_path());
    if (space_info.available < total_size) {
        throw std::runtime_error("Insufficient disk space! Need " + std::to_string(total_size) +
            " bytes, but only " + std::to_string(space_info.available) + " available.");
    }

    std::ofstream out(output_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        int err = errno;
        throw std::runtime_error("Cannot create output file: " + output_file.string() +
            " (error: " + strerror(err) + ")");
    }

    uintmax_t processed_bytes = 0;
    const size_t total_files = files.size();
    for (size_t i = 0; i < total_files; ++i) {
        std::ifstream in(files[i], std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("Cannot open input file: " + files[i].string());
        }

        Policy::apply(in, (i == 0));

        uintmax_t file_size = fs::file_size(files[i]);
        out << in.rdbuf();
        if (out.fail()) {
            throw std::runtime_error("Write failed! Possibly disk full or device error at file: " + files[i].string());
        }
        out.flush();
        if (out.fail()) {
            throw std::runtime_error("Flush failed! Disk full or device error.");
        }

        processed_bytes += file_size;
        {
            std::lock_guard<std::mutex> lock(g_cerr_mutex);
            if (total_size > 0) {
                int percent = static_cast<int>((processed_bytes * 100) / total_size);
                std::cerr << "\rProgress: [" << (i + 1) << "/" << total_files << "] "
                    << percent << "% - " << files[i].filename().string() << "    ";
            }
            else {
                std::cerr << "\rProgress: [" << (i + 1) << "/" << total_files << "] "
                    << files[i].filename().string() << "    ";
            }
            std::cerr.flush();
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::cerr << std::endl;
    }
}

// ------------------- 生成带时间戳的文件名（单文件模式） -------------------
fs::path generate_timestamp_filename(const fs::path& dir, const std::string& prefix = "merged") {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm_buf, &in_time_t);
#else
    localtime_r(&in_time_t, &tm_buf);
#endif
    std::stringstream ss;
    ss << prefix << "_" << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".csv";
    return dir / ss.str();
}

// ------------------- DFS 搜索最优子集（不超过阈值，尽量接近） -------------------
struct DfsResult {
    std::vector<size_t> indices;
    uintmax_t sum;
};

void dfs_best_subset(size_t idx,
    uintmax_t current_sum,
    std::vector<size_t>& chosen,
    DfsResult& best,
    const std::vector<uintmax_t>& sizes,
    const std::vector<uintmax_t>& suffix_sum,
    uintmax_t threshold) {
    if (current_sum > best.sum && current_sum <= threshold) {
        best.sum = current_sum;
        best.indices = chosen;
    }
    if (idx >= sizes.size()) return;

    if (current_sum + suffix_sum[idx] <= best.sum) return;

    // 不选
    dfs_best_subset(idx + 1, current_sum, chosen, best, sizes, suffix_sum, threshold);

    // 选
    if (current_sum + sizes[idx] <= threshold) {
        chosen.push_back(idx);
        dfs_best_subset(idx + 1, current_sum + sizes[idx], chosen, best, sizes, suffix_sum, threshold);
        chosen.pop_back();
    }
}

std::vector<size_t> find_best_group(const std::vector<std::pair<fs::path, uintmax_t>>& small_files,
    uintmax_t threshold) {
    if (small_files.empty()) return {};

    std::vector<uintmax_t> sizes;
    sizes.reserve(small_files.size());
    for (const auto& p : small_files) {
        sizes.push_back(p.second);
    }

    std::vector<uintmax_t> suffix_sum(sizes.size() + 1, 0);
    for (int i = static_cast<int>(sizes.size()) - 1; i >= 0; --i) {
        suffix_sum[i] = suffix_sum[i + 1] + sizes[i];
    }

    DfsResult best;
    best.sum = 0;
    std::vector<size_t> chosen;
    dfs_best_subset(0, 0, chosen, best, sizes, suffix_sum, threshold);

    if (best.indices.empty() && !small_files.empty()) {
        best.indices.push_back(0);
        best.sum = sizes[0];
    }
    return best.indices;
}

// ------------------- 分组算法（DFS + 剪枝，安全删除） -------------------
std::vector<std::vector<fs::path>> group_files_by_size(
    const std::vector<std::pair<fs::path, uintmax_t>>& files,
    uintmax_t threshold)
{
    std::vector<std::vector<fs::path>> groups;

    std::vector<std::pair<fs::path, uintmax_t>> small_files;
    for (const auto& item : files) {
        if (item.second >= threshold) {
            groups.push_back({ item.first });
        }
        else {
            small_files.push_back(item);
        }
    }

    std::sort(small_files.begin(), small_files.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    while (!small_files.empty()) {
        auto best_indices = find_best_group(small_files, threshold);
        if (best_indices.empty()) {
            groups.push_back({ small_files[0].first });
            small_files.erase(small_files.begin());
            continue;
        }

        std::vector<fs::path> group;
        for (size_t idx : best_indices) {
            if (idx >= small_files.size()) {
                throw std::runtime_error("Internal error: invalid index in best_indices");
            }
            group.push_back(small_files[idx].first);
        }
        groups.push_back(std::move(group));

        // 重建剩余列表
        std::vector<std::pair<fs::path, uintmax_t>> remaining;
        for (size_t i = 0; i < small_files.size(); ++i) {
            if (std::find(best_indices.begin(), best_indices.end(), i) == best_indices.end()) {
                remaining.push_back(small_files[i]);
            }
        }
        small_files.swap(remaining);
    }

    return groups;
}

// ------------------- 主函数（支持阈值参数） -------------------
int main(int argc, char* argv[]) {
    try {
        // 默认参数
        std::string mode = "multi";
        uintmax_t threshold_mb = 1024;       // 默认 1GB
        std::string prefix = "merged";

        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " <input_dir> <output_dir> [mode] [threshold_mb] [prefix]\n"
                << "  mode          : 'single' or 'multi', default 'multi'\n"
                << "  threshold_mb  : positive integer (MB), default 1024\n"
                << "  prefix        : output filename prefix, default 'merged'\n";
            return 1;
        }

        fs::path input_dir = argv[1];
        fs::path output_dir = argv[2];

        if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
            std::cerr << "Error: Input directory does not exist or is not a directory." << std::endl;
            return 1;
        }
        if (!fs::exists(output_dir) || !fs::is_directory(output_dir)) {
            std::cerr << "Error: Output directory does not exist or is not a directory." << std::endl;
            return 1;
        }
        if (fs::equivalent(input_dir, output_dir)) {
            std::cerr << "Error: Input and output directories must be different!" << std::endl;
            return 1;
        }

        // 解析可选参数（按顺序）
        if (argc >= 4) {
            mode = argv[3];
        }
        if (argc >= 5) {
            threshold_mb = std::stoull(argv[4]);
            if (threshold_mb == 0) {
                std::cerr << "Error: threshold_mb must be positive." << std::endl;
                return 1;
            }
        }
        if (argc >= 6) {
            prefix = argv[5];
        }

        // 转换为字节（注意溢出检查）
        const uintmax_t threshold_bytes = threshold_mb * 1024ULL * 1024ULL;
        // 简单检查溢出（若阈值小于1MB则可能溢出，但一般不会）
        if (threshold_bytes < threshold_mb) {
            std::cerr << "Error: threshold too large (overflow)." << std::endl;
            return 1;
        }

        std::cout << "Mode: " << mode << ", Threshold: " << threshold_mb << " MB ("
            << threshold_bytes << " bytes), Prefix: " << prefix << std::endl;

        auto file_list = get_csv_files(input_dir);
        if (file_list.empty()) {
            std::cerr << "No CSV files found in input directory." << std::endl;
            return 0;
        }

        using Policy = KeepFirstHeaderPolicy;

        if (mode == "single") {
            std::vector<fs::path> all_files;
            std::transform(file_list.begin(), file_list.end(), std::back_inserter(all_files),
                [](const auto& p) { return p.first; });
            fs::path output_file = generate_timestamp_filename(output_dir, prefix);
            std::cout << "Single file mode: merging all files into " << output_file << std::endl;
            merge_files<Policy>(all_files, output_file);
            std::cout << "Merge completed successfully!" << std::endl;
        }
        else if (mode == "multi") {
            auto groups = group_files_by_size(file_list, threshold_bytes);
            if (groups.empty()) {
                std::cerr << "No groups to process." << std::endl;
                return 1;
            }
            std::cout << "Multi-file mode: " << groups.size() << " groups will be created." << std::endl;

            unsigned int max_threads = std::thread::hardware_concurrency();
            if (max_threads == 0) max_threads = 4;
            std::cout << "Using up to " << max_threads << " threads." << std::endl;

            std::vector<std::future<void>> futures;
            size_t group_count = groups.size();

            for (size_t start = 0; start < group_count; start += max_threads) {
                size_t end = std::min(start + max_threads, group_count);
                futures.clear();

                for (size_t i = start; i < end; ++i) {
                    std::stringstream ss;
                    ss << prefix << "_" << std::setw(3) << std::setfill('0') << (i + 1) << ".csv";
                    fs::path output_file = output_dir / ss.str();

                    if (fs::exists(output_file)) {
                        std::cerr << "Warning: " << output_file << " already exists, will be overwritten." << std::endl;
                    }

                    auto group = std::move(groups[i]);
                    futures.push_back(std::async(std::launch::async,
                        [group = std::move(group), output_file]() {
                            merge_files<Policy>(group, output_file);
                        }
                    ));
                }

                for (auto& fut : futures) {
                    try {
                        fut.get();
                    }
                    catch (const std::exception& e) {
                        throw std::runtime_error(std::string("Thread task failed: ") + e.what());
                    }
                }
            }

            std::cout << "All groups merged successfully!" << std::endl;
        }
        else {
            std::cerr << "Invalid mode: " << mode << ". Use 'single' or 'multi'." << std::endl;
            return 1;
        }

    }
    catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}