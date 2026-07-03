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

// ------------------- 分组算法（多文件模式）修正版 -------------------
std::vector<std::vector<fs::path>> group_files_by_size(
    const std::vector<std::pair<fs::path, uintmax_t>>& files,
    uintmax_t threshold = 1ULL << 30) // 1 GB
{
    std::vector<std::vector<fs::path>> groups;
    std::vector<std::pair<fs::path, uintmax_t>> small_files;
    std::vector<std::pair<fs::path, uintmax_t>> large_files;

    // 分离超大文件（≥ threshold）和小文件
    for (const auto& [path, size] : files) {
        if (size >= threshold) {
            large_files.push_back({ path, size });
        }
        else {
            small_files.push_back({ path, size });
        }
    }

    // 超大文件单独成组
    for (const auto& [path, size] : large_files) {
        groups.push_back({ path });
    }

    // 对小文件按大小降序排序（便于贪心）
    std::sort(small_files.begin(), small_files.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    // 修正后的贪心分组：
    uintmax_t current_sum = 0;
    std::vector<fs::path> current_group;

    for (const auto& [path, size] : small_files) {
        if (current_group.empty()) {
            current_group.push_back(path);
            current_sum = size;
        }
        else {
            // 如果加上当前文件会达到或超过阈值，则将当前文件加入并关闭组
            if (current_sum + size >= threshold) {
                current_group.push_back(path);
                current_sum += size;
                groups.push_back(std::move(current_group));
                current_group.clear();
                current_sum = 0;
            }
            else {
                current_group.push_back(path);
                current_sum += size;
            }
        }
    }

    // 处理最后一组（即使小于阈值也单独输出，不再并入前一组）
    if (!current_group.empty()) {
        groups.push_back(std::move(current_group));
    }

    return groups;
}

// ------------------- 主函数 -------------------
int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " <input_dir> <output_dir> [mode] [prefix]\n"
                << "  mode: 'single' (merge all into one) or 'multi' (group by size), default 'multi'\n"
                << "  prefix: output filename prefix, default 'merged'\n";
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

        std::string mode = "multi";
        std::string prefix = "merged";
        if (argc >= 4) {
            mode = argv[3];
        }
        if (argc >= 5) {
            prefix = argv[4];
        }

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
            auto groups = group_files_by_size(file_list, 1ULL << 30);
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