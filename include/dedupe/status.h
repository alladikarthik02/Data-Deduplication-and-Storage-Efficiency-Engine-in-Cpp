// Error propagation for the I/O and format layers.
//
// WHY NOT EXCEPTIONS:
//   Every function here is on a path where an error is an ORDINARY outcome, not
//   an exceptional one: a short write, a torn record, a corrupt header, a full
//   disk. Exceptions make those paths invisible at the call site, and the whole
//   point of SPEC S11 ("no silent failure") is that an unhandled error should be
//   impossible to write, not merely discouraged.
//
// WHY [[nodiscard]] IS THE LOAD-BEARING PART:
//   Status and Result are both [[nodiscard]], so ignoring a return value is a
//   COMPILE ERROR, not a code-review finding. Combined with -Werror=unused-result
//   in CMakeLists, "I forgot to check whether the fsync worked" cannot reach the
//   repository. That is S11 enforced by the type system rather than by discipline.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace dedupe {

enum class Err : uint8_t {
  kOk = 0,
  kIo,        // a syscall failed; errno carried alongside
  kCorrupt,   // data on disk failed validation (magic, CRC, fingerprint, bounds)
  kNotFound,
  kExists,
  kInvalid,   // caller passed something impossible
  kNoSpace,
  kLocked,    // another process holds the store lock (S14)
  kInjected,  // deliberate test fault (S11 fault injection)
};

inline const char* err_name(Err e) {
  switch (e) {
    case Err::kOk: return "ok";
    case Err::kIo: return "io";
    case Err::kCorrupt: return "corrupt";
    case Err::kNotFound: return "not_found";
    case Err::kExists: return "exists";
    case Err::kInvalid: return "invalid";
    case Err::kNoSpace: return "no_space";
    case Err::kLocked: return "locked";
    case Err::kInjected: return "injected";
  }
  return "unknown";
}

class [[nodiscard]] Status {
 public:
  Status() = default;  // success

  static Status error(Err e, std::string msg, int err_no = 0) {
    Status s;
    s.code_ = e;
    s.errno_ = err_no;
    s.msg_ = std::move(msg);
    return s;
  }
  // Convenience for the common "a syscall just failed" case.
  static Status from_errno(std::string what) {
    return error(errno == ENOSPC ? Err::kNoSpace : Err::kIo, std::move(what), errno);
  }
  static Status corrupt(std::string msg) { return error(Err::kCorrupt, std::move(msg)); }
  static Status invalid(std::string msg) { return error(Err::kInvalid, std::move(msg)); }

  bool ok() const { return code_ == Err::kOk; }
  Err code() const { return code_; }
  int err_no() const { return errno_; }
  const std::string& message() const { return msg_; }

  std::string to_string() const {
    if (ok()) return "ok";
    std::string s = std::string(err_name(code_)) + ": " + msg_;
    if (errno_ != 0) s += " (errno " + std::to_string(errno_) + ": " + std::strerror(errno_) + ")";
    return s;
  }

 private:
  Err code_ = Err::kOk;
  int errno_ = 0;
  std::string msg_;  // stays empty on the success path, so ok() costs nothing
};

// A value, or the reason there isn't one.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T v) : value_(std::move(v)) {}          // NOLINT: implicit on purpose
  Result(Status s) : status_(std::move(s)) {}    // NOLINT
  Result(const Result&) = delete;                // move-only: values here own fds
  Result& operator=(const Result&) = delete;
  Result(Result&&) = default;
  Result& operator=(Result&&) = default;

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  // Precondition: ok(). Calling this on an error Result is a programming bug,
  // and the tests never do it -- REQUIRE(r.ok()) comes first.
  T& value() { return value_; }
  const T& value() const { return value_; }
  T&& take() { return std::move(value_); }

 private:
  Status status_;
  T value_{};
};

#define DEDUPE_CAT_(a, b) a##b
#define DEDUPE_CAT2_(a, b) DEDUPE_CAT_(a, b)

// Propagate an error, or fall through. The named temporary avoids evaluating
// `expr` twice and keeps the failure's message intact.
#define RETURN_IF_ERROR(expr)                        \
  do {                                               \
    ::dedupe::Status DEDUPE_CAT2_(_st_, __LINE__) = (expr); \
    if (!DEDUPE_CAT2_(_st_, __LINE__).ok()) return DEDUPE_CAT2_(_st_, __LINE__); \
  } while (0)

// Unwrap a Result into `lhs`, or propagate its error.
#define ASSIGN_OR_RETURN(lhs, expr)                                    \
  auto DEDUPE_CAT2_(_res_, __LINE__) = (expr);                         \
  if (!DEDUPE_CAT2_(_res_, __LINE__).ok())                             \
    return DEDUPE_CAT2_(_res_, __LINE__).status();                     \
  lhs = DEDUPE_CAT2_(_res_, __LINE__).take()

}  // namespace dedupe
