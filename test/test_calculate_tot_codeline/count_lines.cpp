#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FileInfo {
	std::string path;
	std::size_t lines;
};

std::size_t count_lines(const std::string &filepath) {
	std::ifstream in(filepath);
	if (!in) return 0;
	std::size_t n = 0;
	std::string line;
	while (std::getline(in, line)) ++n;
	return n;
}

bool is_source(const std::string &path) {
	return path.find("src/") != std::string::npos ||
		   path.find("include/") != std::string::npos;
}

bool is_test(const std::string &path) {
	return path.find("test/") != std::string::npos;
}

bool is_cpp_or_h(const std::string &path) {
	auto ends_with = [](const std::string &s, const std::string &suffix) {
		return s.size() >= suffix.size() &&
			   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
	};
	return ends_with(path, ".cpp") || ends_with(path, ".h");
}

int main(int argc, char **argv) {
	std::string root = ".";
	if (argc > 1) root = argv[1];

	std::vector<FileInfo> src_files, test_files;
	std::size_t src_total = 0, test_total = 0;

	try {
		for (const auto &entry :
			 fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) continue;
			std::string p = entry.path().string();
			if (!is_cpp_or_h(p)) continue;
			if (p.find("/build") != std::string::npos) continue;
			if (p.find("/.tmp") != std::string::npos) continue;
			if (p.find("/source_code_backup") != std::string::npos) continue;
			if (p.find("/.codex") != std::string::npos) continue;
			if (p.find("test_calculate_tot_codeline") != std::string::npos) continue;

			std::size_t lines = count_lines(p);

			if (is_source(p)) {
				src_files.push_back({p, lines});
				src_total += lines;
			} else if (is_test(p)) {
				test_files.push_back({p, lines});
				test_total += lines;
			}
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << '\n';
		return 1;
	}

	auto print_files = [](const char *title,
						  const std::vector<FileInfo> &files,
						  std::size_t total) {
		std::cout << "\n━━━ " << title << " ━━━\n";
		for (const auto &f : files) {
			std::cout << "  " << f.lines << "\t" << f.path << '\n';
		}
		std::cout << "  ──────────\n";
		std::cout << "  " << total << "\ttotal\n";
	};

	print_files("Source Code", src_files, src_total);
	print_files("Test Code", test_files, test_total);

	std::cout << "\n══════════════════════════\n";
	std::cout << "  Source: " << src_total << " lines (" << src_files.size()
			  << " files)\n";
	std::cout << "  Tests:  " << test_total << " lines (" << test_files.size()
			  << " files)\n";
	std::cout << "  Total:  " << (src_total + test_total) << " lines ("
			  << (src_files.size() + test_files.size()) << " files)\n";

	return 0;
}
