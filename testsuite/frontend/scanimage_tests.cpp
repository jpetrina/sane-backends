#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>

#include "minigtest/minigtest.h"

namespace {

/**
 * A temporary file that automatically deletes itself when it goes out of scope.
 * This type is move-only to try to ensure that only one instance is responsible
 * for unlink()ing the file.
 *
 * The purpose of the temporary file is to pass it by name to subprocesses of
 * scanimage. For this reason, we couldn't tmpfile(3), because there's no
 * portable way to obtain the file's name from a file descriptor.
 */
class TempFile {
public:
  static TempFile create() {
    char tmpl[]{"outfile.XXXXXX"};
    int fd = mkstemp(tmpl);
    if (fd == -1) {
      std::cerr << "could not create temporary file: " << strerror(errno)
                << "\n";
      return TempFile("");
    }

    return TempFile(std::string(tmpl));
  }

  explicit TempFile(std::string name) : name_(std::move(name)) {}

  TempFile(const TempFile &other) = delete;

  TempFile(TempFile &&other) noexcept {
    name_ = other.name_;
    other.name_ = "";
  }

  ~TempFile() {
    if (!name_.empty() && unlink(name_.c_str()) == -1) {
      std::cerr << "Failed to unlink " << name_ << ": " << strerror(errno)
                << "\n";
    }
  }

  std::string name() { return name_; }

private:
  std::string name_;
};
} // namespace

/**
 * Runs the `scanimage` frontend with the given arguments. If `timeout_sec > 0`,
 * this function will wait no more than approximately `timeout_sec` seconds for
 * scanimage to finish.
 */
int scanimage(std::initializer_list<std::string> args, uint timeout_sec = 5) {
  const char **str_args = new const char *[args.size() + 2];
  size_t i = 0;
  str_args[i++] = "scanimage";
  for (const std::string &arg : args) {
    str_args[i++] = arg.c_str();
  }
  str_args[i] = nullptr;

  int pid = fork();
  if (pid == -1) {
    std::cerr << "fork() failed: " << strerror(errno) << "\n";
    ASSERT_TRUE(false);
  } else if (pid == 0) {
    ASSERT_NE(execv("../../frontend/scanimage", (char **)str_args), -1);
    return -1;
  } else {
    std::cerr << "Ran `scanimage";
    for (const std::string &arg : args) {
      std::cerr << " " << arg;
    }
    std::cerr << "` with pid=" << pid << "\n";
  }
  delete[] str_args;

  std::chrono::system_clock::time_point start =
      std::chrono::system_clock::now();
  while (true) {
    int res;
    pid_t finished_pid = waitpid(pid, &res, timeout_sec > 0 ? WNOHANG : 0);

    if (finished_pid == -1) {
      std::cerr << "waitpid(" << pid << ", ...) failed: " << strerror(errno)
                << "\n";
      ASSERT_TRUE(false);
    }
    if (finished_pid == pid) {
      return res;
    }

    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_sec = now - start;
    if (elapsed_sec.count() > timeout_sec) {
      std::cerr << elapsed_sec.count() << "\n";
      std::cerr << "scanimage timed out after " << timeout_sec << "s\n";
      ASSERT_TRUE(false);
      return -1;
    }

    if (usleep(50000 /* 50ms */) == -1) {
      std::cerr << "usleep failed: " << strerror(errno) << "\n";
      ASSERT_FALSE(true);
      return -1;
    }
  }
}

void test_flatbed() {
  TempFile outfile = TempFile::create();
  ASSERT_EQ(
      scanimage({"-d", "test", "-T", "--format", "pnm", "-o", outfile.name()}),
      0);
}

void test_three_pass_scanner() {
  TempFile outfile = TempFile::create();
  ASSERT_EQ(scanimage({"-d", "test", "--mode", "Color", "--three-pass=yes",
                       "-T", "--format", "pnm", "-o", outfile.name()}),
            0);
}

void test_hand_scanner() {
  TempFile outfile = TempFile::create();
  ASSERT_EQ(scanimage({"-d", "test", "--hand-scanner=yes", "-T", "--format",
                       "pnm", "-o", outfile.name()}),
            0);
}

void test_16_bit_color() {
  TempFile outfile = TempFile::create();
  ASSERT_EQ(
      scanimage({"-d", "test", "--mode", "Color", "--depth", "16",
                 "--test-picture", "Color pattern", "--resolution", "50", "-y",
                 "20", "-x", "20", "--format", "pnm", "-o", outfile.name()}),
      0);
}

int main() {
  test_flatbed();
  test_three_pass_scanner();
  test_hand_scanner();
  test_16_bit_color();
  return finish_tests();
}
