#pragma once
#include "zco/io/descriptor.h"
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace zhttp::test {
struct TempFile {
    TempFile() {
        char pattern[] = "/tmp/zhttp-test-file-XXXXXX";
        const int fd = ::mkstemp(pattern);
        if (fd < 0)
            throw std::runtime_error("mkstemp failed");
        try {
            descriptor = zco::io::Descriptor(fd);
            path = pattern;
        } catch (...) {
            ::unlink(pattern);
            throw;
        }
    }

    ~TempFile() { ::unlink(path.c_str()); }

    std::string path;
    zco::io::Descriptor descriptor;
};
} // namespace zhttp::test
