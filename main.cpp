#define  _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <numeric>
#include <limits>


namespace fs = std::filesystem;

struct  KeepFirstHeaderPolicy {
	static void apply(std::ifstream& in, bool is_first_line) {
		if (!is_first_line) {
			in.ignore(std::numeric_limits<std::streamsize>::max(),'\n');
		}
	}
};


struct DropAllHeadersPolicy {
	static  void apply(std::ifstream & in, bool /*is_first_line*/) {
		in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	}
};

struct  NoHeadersPolicy {
	static void apply(std::ifstream& /*in*/, bool /*is_first_line*/) {
		/**/
	}
};



fs::path generate_unique_filename(const fs::path& dir, const std::string& prefix = "merged", const std::string& ext = ".csv") {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    // 使用 std::localtime（线程安全版本在某些平台需用 localtime_r）
    std::tm tm_buf;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm_buf, &in_time_t);
#else
    localtime_r(&in_time_t, &tm_buf);  // POSIX
#endif
    std::stringstream ss;
    ss << prefix << "_"
        << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ext;
    return dir / ss.str();
}



template<typename Policy>
void mergeCsv(const fs::path& input_dir, const fs::path& output_file) {
    std::vector<fs::path> csv_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".csv") {
            csv_files.push_back(entry.path());
        }
    }
    if (csv_files.empty()) {
        throw std::runtime_error("No CSV files found in input directory.");
    }
    std::sort(csv_files.begin(), csv_files.end());

    uintmax_t total_size = 0;
    for (const auto& p : csv_files) {
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
    const size_t total_files = csv_files.size();


    for (size_t i = 0; i < csv_files.size(); ++i) {
        std::ifstream in(csv_files[i], std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("Cannot open input file: " + csv_files[i].string());
        }
        Policy::apply(in, (i == 0));
        uintmax_t file_size = fs::file_size(csv_files[i]);
        out << in.rdbuf();


        if (out.fail()) {
            throw std::runtime_error("Write failed! Possibly disk full or device error at file: " + csv_files[i].string());
        }
        out.flush();
        if (out.fail()) {
            throw std::runtime_error("Flush failed! Disk full or device error.");
        }

        processed_bytes += file_size;
        if (total_size > 0) {
            int percent = static_cast<int>((processed_bytes * 100) / total_size);
            std::cerr << "\rProgress: [" << (i + 1) << "/" << total_files << "] "
                << percent << "% - " << csv_files[i].filename().string() << "    ";
        }
        else {
            std::cerr << "\rProgress: [" << (i + 1) << "/" << total_files << "] "
                << csv_files[i].filename().string() << "    ";
        }
        std::cerr.flush();
    }
    std::cerr << std::endl;
}


int main(int argc, char** argv) {
	
	try {
		if (argc != 3) {
			std::cerr << "Usage: " << argv[0] << " <input_directory> <output_directory>" << std::endl;
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
        std::string prefix = (argc >= 4) ? argv[3] : "merged";
        fs::path output_file = generate_unique_filename(output_dir, prefix);
        std::cout << "Output file: " << output_file << std::endl;

        mergeCsv<KeepFirstHeaderPolicy>(input_dir, output_file);
        std::cout << "Merge completed successfully!" << std::endl;
	}
	catch (const std::exception& e) {
		std::cerr << "Fatal Error: " << e.what() << std::endl;
		return 1;
	}

	return  0;
}










