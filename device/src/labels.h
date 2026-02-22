#pragma once
#include <string>
#include <vector>
#include <fstream>

inline std::vector<std::string> load_labels(const std::string& path) {
    std::vector<std::string> labels;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) labels.push_back(line);
    return labels;
}
