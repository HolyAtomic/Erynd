#pragma once

#include <discordcpp/transport.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

class FakeTransport final : public discordcpp::IHttpTransport {
public:
    void queue_response(discordcpp::HttpResponse response) {
        std::lock_guard lock{ mutex_ };
        queued_results_.emplace_back(discordcpp::TransportResult::success(std::move(response)));
    }

    void queue_error(discordcpp::TransportError error) {
        std::lock_guard lock{ mutex_ };
        queued_results_.emplace_back(discordcpp::TransportResult::failure(std::move(error)));
    }

    void set_before_next_return(std::function<void()> action) {
        std::lock_guard lock{ mutex_ };
        before_next_return_ = std::move(action);
    }

    discordcpp::TransportResult send(const discordcpp::HttpRequest& request, std::stop_token) override {
        std::unique_lock lock{ mutex_ };
        request_order_.emplace_back(request.target);
        requests_.emplace_back(request);
        if (queued_results_.empty()) {
            return discordcpp::TransportResult::failure({
                discordcpp::TransportFailureCategory::protocol,
                "fake transport response queue is empty",
                false
            });
        }
        auto result = std::move(queued_results_.front());
        queued_results_.pop_front();
        auto action = std::move(before_next_return_);
        before_next_return_ = {};
        lock.unlock();
        if (action) {
            action();
        }
        return result;
    }

    [[nodiscard]] std::size_t request_count() const {
        std::lock_guard lock{ mutex_ };
        return requests_.size();
    }

    [[nodiscard]] std::vector<discordcpp::HttpRequest> requests() const {
        std::lock_guard lock{ mutex_ };
        return requests_;
    }

    [[nodiscard]] std::vector<std::string> request_order() const {
        std::lock_guard lock{ mutex_ };
        return request_order_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<discordcpp::TransportResult> queued_results_;
    std::vector<discordcpp::HttpRequest> requests_;
    std::vector<std::string> request_order_;
    std::function<void()> before_next_return_;
};

} // namespace test_support
