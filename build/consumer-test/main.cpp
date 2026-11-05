#include <discordcpp/rest_client.hpp>
#include <discordcpp/rate_limit.hpp>
#include <discordcpp/transport.hpp>

#include <optional>

class ConsumerTransport final : public discordcpp::IHttpTransport {
public:
    discordcpp::TransportResult send(const discordcpp::HttpRequest&, std::stop_token) override {
        return discordcpp::TransportResult::success({ 200, {}, "consumer" });
    }
};

int main() {
    ConsumerTransport transport;
    discordcpp::SteadyRateLimitScheduler scheduler;
    discordcpp::RateLimitCoordinator coordinator{ discordcpp::RateLimitIdentity{ 1 }, scheduler };
    discordcpp::RestClient client{ transport };
    const discordcpp::HttpRequest request{ "GET", "/channels/123", {}, {}, std::nullopt };
    const discordcpp::RouteDescriptor route{ "GET", "/channels/{channel_id}",
        { { discordcpp::MajorResourceKind::channel, "123", 0 } } };
    const auto result = client.execute(request, route, coordinator);
    return result.has_value() && result.value().body == "consumer" ? 0 : 1;
}
