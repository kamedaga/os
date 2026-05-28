#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

struct Component {
    std::string name;
    int score;
};

static unsigned long checksum(const std::string& text) {
    unsigned long hash = 5381;
    for (unsigned char ch : text) {
        hash = ((hash << 5) + hash) ^ ch;
    }
    return hash;
}

static std::string render_line(std::size_t index, const Component& component) {
    std::ostringstream out;
    out << (index + 1)
        << ',' << component.name
        << ',' << component.score
        << ',' << checksum(component.name);
    return out.str();
}

static bool write_report(const std::string& path, const std::vector<Component>& components) {
    std::ofstream file(path);
    if (!file) {
        std::cerr << "open failed: " << path << '\n';
        return false;
    }

    for (std::size_t i = 0; i < components.size(); ++i) {
        file << render_line(i, components[i]) << '\n';
    }
    return true;
}

static bool read_report(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "read failed: " << path << '\n';
        return false;
    }

    std::cout << "report:\n";
    std::string line;
    while (std::getline(file, line)) {
        std::cout << line << '\n';
    }
    return true;
}

int main() {
    std::vector<Component> components = {
        { "libstdc++", 96 },
        { "iostream", 89 },
        { "vector", 93 },
        { "fstream", 87 },
        { "sort", 91 },
    };

    std::sort(components.begin(), components.end(), [](const Component& a, const Component& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });

    const int total = std::accumulate(
        components.begin(),
        components.end(),
        0,
        [](int sum, const Component& item) { return sum + item.score; });

    const double average = static_cast<double>(total) / static_cast<double>(components.size());
    std::cout << "items=" << components.size()
              << " average=" << std::fixed << std::setprecision(2) << average
              << " best=" << components.front().name << '\n';

    const std::string path = "/tmp/clangxx-report.txt";
    if (!write_report(path, components)) return 1;
    if (!read_report(path)) return 1;
    return 0;
}
