#pragma once
// Result<T>: a small expected-like type. Engine APIs that can fail (parsing, IO,
// agent tool calls) return Result instead of throwing, so failures can be turned
// into structured, agent-readable errors at the boundary.

#include <string>
#include <utility>
#include <variant>

namespace sky {

struct Error {
    std::string code;     // stable, machine-readable (e.g. "not_found", "parse_error")
    std::string message;  // human/agent readable explanation
    std::string hint;     // optional suggestion for how to fix it

    static Error make(std::string code, std::string message, std::string hint = {}) {
        return Error{std::move(code), std::move(message), std::move(hint)};
    }
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::move(value)) {}           // NOLINT(google-explicit-constructor)
    Result(Error error) : data_(std::move(error)) {}       // NOLINT(google-explicit-constructor)

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    T& value() & { return std::get<T>(data_); }
    const T& value() const& { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }
    const Error& error() const { return std::get<Error>(data_); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> data_;
};

/// Result<void> equivalent.
class [[nodiscard]] Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT(google-explicit-constructor)
    static Status success() { return {}; }

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const Error& error() const { return error_; }

private:
    Error error_;
    bool ok_ = true;
};

}  // namespace sky
