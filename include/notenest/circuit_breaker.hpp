#ifndef CIRCUIT_BREAKER_HPP
#define CIRCUIT_BREAKER_HPP

#include <chrono>
#include <mutex>
#include <string>

// State of the Circuit Breaker.
enum class CircuitState { CLOSED, OPEN, HALF_OPEN };

// Circuit Breaker wrapper to protect remote operations and fail fast.
class CircuitBreaker {
public:
    // Initializes circuit breaker with failure threshold and reset timeout in milliseconds.
    CircuitBreaker(size_t failure_threshold = 5,
                   std::chrono::milliseconds reset_timeout = std::chrono::milliseconds(5000))
        : failure_threshold_(failure_threshold), reset_timeout_(reset_timeout) {}

    // Checks whether a request is allowed based on the current state.
    bool allowRequest() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        if (state_ == CircuitState::OPEN) {
            if (now - last_state_change_ >= reset_timeout_) {
                state_ = CircuitState::HALF_OPEN;
                last_state_change_ = now;
                return true;
            }
            return false;
        }
        return true;
    }

    // Records a successful operation execution.
    void recordSuccess() {
        std::lock_guard<std::mutex> lock(mutex_);
        consecutive_failures_ = 0;
        if (state_ == CircuitState::HALF_OPEN) {
            state_ = CircuitState::CLOSED;
            last_state_change_ = std::chrono::steady_clock::now();
        }
    }

    // Records a failed operation execution.
    void recordFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        consecutive_failures_++;
        auto now = std::chrono::steady_clock::now();
        if (state_ == CircuitState::CLOSED && consecutive_failures_ >= failure_threshold_) {
            state_ = CircuitState::OPEN;
            last_state_change_ = now;
        } else if (state_ == CircuitState::HALF_OPEN) {
            state_ = CircuitState::OPEN;
            last_state_change_ = now;
        }
    }

    // Returns current circuit state.
    CircuitState getState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    // Returns string representation of circuit state.
    std::string getStateName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (state_) {
            case CircuitState::CLOSED:
                return "CLOSED";
            case CircuitState::OPEN:
                return "OPEN";
            case CircuitState::HALF_OPEN:
                return "HALF_OPEN";
        }
        return "UNKNOWN";
    }

private:
    size_t failure_threshold_;
    std::chrono::milliseconds reset_timeout_;
    CircuitState state_ = CircuitState::CLOSED;
    size_t consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point last_state_change_ = std::chrono::steady_clock::now();
    mutable std::mutex mutex_;
};

#endif
