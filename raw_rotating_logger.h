#pragma once
#include <string>
#include <mutex>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

class RotatingFileWriter {
public:
    RotatingFileWriter(std::string base_name,
                       size_t max_bytes,
                       size_t max_files)
        : base_name_(std::move(base_name)),
          max_bytes_(max_bytes),
          max_files_(max_files) {
        openFile();
    }

    ~RotatingFileWriter() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0) ::close(fd_);
    }

    // Thread-safe raw write
    void write(const char* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        ssize_t bytes_written;

        // check for rotation
        off_t sz = currentSizeUnlocked();
        if (sz + static_cast<off_t>(size) > static_cast<off_t>(max_bytes_)) {
            rotateFilesUnlocked();
            openFileUnlocked();
        }

        // POSIX atomic append write
        bytes_written = ::write(fd_, data, size);
        if (bytes_written != (ssize_t)size) {
            printf("[warning]logger write requested:%ld, actual:%ld\n", size, bytes_written);
        }
    }

    void write(const std::string& s) {
        write(s.data(), s.size());
    }

private:
    std::string base_name_;
    size_t max_bytes_;
    size_t max_files_;

    int fd_ = -1;
    std::mutex mutex_;

private:
    // ---- helpers ----
    off_t currentSizeUnlocked() {
        if (fd_ < 0) return 0;
        struct stat st;
        if (::fstat(fd_, &st) != 0) return 0;
        return st.st_size;
    }

    void openFile() {
        std::lock_guard<std::mutex> lock(mutex_);
        openFileUnlocked();
    }

    void openFileUnlocked() {
        if (fd_ >= 0) ::close(fd_);

        // O_APPEND guarantees atomic writes
        fd_ = ::open(base_name_.c_str(),
                     O_WRONLY | O_CREAT | O_APPEND,
                     0666);

        // ensure file exists
        if (fd_ < 0) {
            throw std::runtime_error("Failed to open " + base_name_);
        }
    }

    void rotateFilesUnlocked() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }

        // delete oldest
        std::string oldest = base_name_ + "." + std::to_string(max_files_ - 1);
        if (std::filesystem::exists(oldest)) {
            std::filesystem::remove(oldest);
        }

        // shift suffix N-1 -> N
        for (size_t i = max_files_ - 1; i-- > 0; ) {
            std::string src = (i == 0)
                                ? base_name_
                                : base_name_ + "." + std::to_string(i);

            if (std::filesystem::exists(src)) {
                std::string dst = base_name_ + "." + std::to_string(i + 1);
                std::filesystem::rename(src, dst);
            }
        }
    }
};
