#pragma once
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlog::test {
class TempDirectory {
  public:
    TempDirectory() {
        char pattern[] = "/tmp/zlynx-log-test-XXXXXX";
        auto *created = ::mkdtemp(pattern);
        if (!created)
            throw std::runtime_error("mkdtemp failed");
        try {
            path_ = created;
        } catch (...) {
            std::error_code error;
            std::filesystem::remove_all(created, error);
            throw;
        }
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

inline std::vector<std::string> listDir(const std::string &path) {
    std::vector<std::string> names;
    for (const auto &entry : std::filesystem::directory_iterator(path))
        names.push_back(entry.path().filename().string());
    return names;
}
} // namespace zlog::test
