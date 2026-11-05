#include <discordcpp/rest_client.hpp>
#include <discordcpp/rate_limit.hpp>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace discordcpp {
namespace {

struct ParsedDiscordError {
    std::optional<std::int64_t> code;
    std::optional<std::string> message;
};

void skip_whitespace(std::string_view input, std::size_t& position) noexcept {
    while (position < input.size()) {
        const char value = input[position];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            break;
        }
        ++position;
    }
}

bool consume(std::string_view input, std::size_t& position, char expected) noexcept {
    if (position >= input.size() || input[position] != expected) {
        return false;
    }
    ++position;
    return true;
}

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7f) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

bool parse_hex_quad(std::string_view input, std::size_t& position, std::uint32_t& value) noexcept {
    if (input.size() - position < 4) {
        return false;
    }
    value = 0;
    for (int index = 0; index < 4; ++index) {
        const char digit = input[position++];
        value <<= 4;
        if (digit >= '0' && digit <= '9') {
            value |= static_cast<std::uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value |= static_cast<std::uint32_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            value |= static_cast<std::uint32_t>(digit - 'A' + 10);
        } else {
            return false;
        }
    }
    return true;
}

bool parse_json_string(std::string_view input, std::size_t& position, std::string& output) {
    if (!consume(input, position, '"')) {
        return false;
    }
    output.clear();
    while (position < input.size()) {
        const auto byte = static_cast<unsigned char>(input[position++]);
        if (byte == '"') {
            return true;
        }
        if (byte < 0x20) {
            return false;
        }
        if (byte != '\\') {
            output.push_back(static_cast<char>(byte));
            continue;
        }
        if (position >= input.size()) {
            return false;
        }
        switch (input[position++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t code_point{};
                if (!parse_hex_quad(input, position, code_point)) {
                    return false;
                }
                if (code_point >= 0xd800 && code_point <= 0xdbff) {
                    if (input.size() - position < 6 || input[position] != '\\' || input[position + 1] != 'u') {
                        return false;
                    }
                    position += 2;
                    std::uint32_t low_surrogate{};
                    if (!parse_hex_quad(input, position, low_surrogate) || low_surrogate < 0xdc00 || low_surrogate > 0xdfff) {
                        return false;
                    }
                    code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low_surrogate - 0xdc00);
                } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
                    return false;
                }
                append_utf8(output, code_point);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool parse_json_number(std::string_view input, std::size_t& position, std::string_view& token) noexcept {
    const auto start = position;
    if (position < input.size() && input[position] == '-') {
        ++position;
    }
    if (position >= input.size()) {
        return false;
    }
    if (input[position] == '0') {
        ++position;
        if (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            return false;
        }
    } else if (input[position] >= '1' && input[position] <= '9') {
        do {
            ++position;
        } while (position < input.size() && input[position] >= '0' && input[position] <= '9');
    } else {
        return false;
    }
    if (position < input.size() && input[position] == '.') {
        ++position;
        const auto fraction_start = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            ++position;
        }
        if (position == fraction_start) {
            return false;
        }
    }
    if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
        ++position;
        if (position < input.size() && (input[position] == '+' || input[position] == '-')) {
            ++position;
        }
        const auto exponent_start = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
            ++position;
        }
        if (position == exponent_start) {
            return false;
        }
    }
    token = input.substr(start, position - start);
    return true;
}

bool skip_json_value(std::string_view input, std::size_t& position, unsigned depth);

bool skip_json_object(std::string_view input, std::size_t& position, unsigned depth) {
    if (depth > 64 || !consume(input, position, '{')) {
        return false;
    }
    skip_whitespace(input, position);
    if (consume(input, position, '}')) {
        return true;
    }
    while (position < input.size()) {
        std::string ignored_key;
        if (!parse_json_string(input, position, ignored_key)) {
            return false;
        }
        skip_whitespace(input, position);
        if (!consume(input, position, ':')) {
            return false;
        }
        skip_whitespace(input, position);
        if (!skip_json_value(input, position, depth + 1)) {
            return false;
        }
        skip_whitespace(input, position);
        if (consume(input, position, '}')) {
            return true;
        }
        if (!consume(input, position, ',')) {
            return false;
        }
        skip_whitespace(input, position);
    }
    return false;
}

bool skip_json_array(std::string_view input, std::size_t& position, unsigned depth) {
    if (depth > 64 || !consume(input, position, '[')) {
        return false;
    }
    skip_whitespace(input, position);
    if (consume(input, position, ']')) {
        return true;
    }
    while (position < input.size()) {
        if (!skip_json_value(input, position, depth + 1)) {
            return false;
        }
        skip_whitespace(input, position);
        if (consume(input, position, ']')) {
            return true;
        }
        if (!consume(input, position, ',')) {
            return false;
        }
        skip_whitespace(input, position);
    }
    return false;
}

bool skip_json_value(std::string_view input, std::size_t& position, unsigned depth) {
    if (depth > 64 || position >= input.size()) {
        return false;
    }
    if (input[position] == '"') {
        std::string ignored;
        return parse_json_string(input, position, ignored);
    }
    if (input[position] == '{') {
        return skip_json_object(input, position, depth);
    }
    if (input[position] == '[') {
        return skip_json_array(input, position, depth);
    }
    for (const auto literal : { std::string_view{"true"}, std::string_view{"false"}, std::string_view{"null"} }) {
        if (input.substr(position, literal.size()) == literal) {
            position += literal.size();
            return true;
        }
    }
    std::string_view number;
    return parse_json_number(input, position, number);
}

std::optional<ParsedDiscordError> parse_discord_error(std::string_view body) {
    ParsedDiscordError parsed;
    std::size_t position{};
    skip_whitespace(body, position);
    if (!consume(body, position, '{')) {
        return std::nullopt;
    }
    skip_whitespace(body, position);
    if (consume(body, position, '}')) {
        skip_whitespace(body, position);
        return position == body.size() ? std::optional<ParsedDiscordError>{ parsed } : std::nullopt;
    }

    while (position < body.size()) {
        std::string key;
        if (!parse_json_string(body, position, key)) {
            return std::nullopt;
        }
        skip_whitespace(body, position);
        if (!consume(body, position, ':')) {
            return std::nullopt;
        }
        skip_whitespace(body, position);

        if (key == "code" && !parsed.code.has_value() && position < body.size() &&
            (body[position] == '-' || (body[position] >= '0' && body[position] <= '9'))) {
            std::string_view number;
            if (!parse_json_number(body, position, number)) {
                return std::nullopt;
            }
            if (number.find_first_of(".eE") == std::string_view::npos) {
                std::int64_t code{};
                const auto conversion = std::from_chars(number.data(), number.data() + number.size(), code);
                if (conversion.ec == std::errc{} && conversion.ptr == number.data() + number.size()) {
                    parsed.code = code;
                }
            }
        } else if (key == "message" && !parsed.message.has_value() && position < body.size() && body[position] == '"') {
            std::string message;
            if (!parse_json_string(body, position, message)) {
                return std::nullopt;
            }
            parsed.message = std::move(message);
        } else if (!skip_json_value(body, position, 0)) {
            return std::nullopt;
        }

        skip_whitespace(body, position);
        if (consume(body, position, '}')) {
            skip_whitespace(body, position);
            return position == body.size() ? std::optional<ParsedDiscordError>{ std::move(parsed) } : std::nullopt;
        }
        if (!consume(body, position, ',')) {
            return std::nullopt;
        }
        skip_whitespace(body, position);
    }
    return std::nullopt;
}

RestResult transport_failure(TransportFailureCategory category, std::string message, bool may_have_reached) {
    RestError error{ std::in_place_type<TransportError>, category, std::move(message), may_have_reached };
    return RestResult::failure(std::move(error));
}

DiscordApiError make_api_error(const HttpResponse& response) {
    DiscordApiError error{};
    error.http_status = response.status;
    error.raw_body = response.body;
    error.response_headers = response.headers;
    error.message = response.body.empty()
        ? "HTTP request failed with status " + std::to_string(response.status)
        : response.body;
    if (const auto parsed = parse_discord_error(response.body); parsed.has_value()) {
        if (parsed->code.has_value()) {
            error.discord_code = parsed->code;
        }
        if (parsed->message.has_value()) {
            error.message = *parsed->message;
        }
    }
    return error;
}

} // namespace

RestClient::RestClient(IHttpTransport& transport, ClockNowFunction now) noexcept
    : transport_{ transport }, now_{ now != nullptr ? now : &SteadyClock::now } {}

RestResult RestClient::execute(const HttpRequest& request, std::stop_token cancellation) const {
    if (cancellation.stop_requested()) {
        return transport_failure(TransportFailureCategory::cancelled, "request cancelled before transport execution", false);
    }
    if (request.deadline.has_value() && now_() >= *request.deadline) {
        return transport_failure(TransportFailureCategory::timeout, "request deadline expired before transport execution", false);
    }

    auto transport_result = transport_.send(request, cancellation);
    switch (transport_result.state()) {
        case ResultState::error: {
            RestError error{ std::in_place_type<TransportError>, std::move(transport_result).error() };
            return RestResult::failure(std::move(error));
        }
        case ResultState::valueless:
            return transport_failure(TransportFailureCategory::protocol, "transport returned a valueless result", true);
        case ResultState::success:
            break;
    }

    // Cancellation is deliberately not rechecked here: a response returned by
    // the transport wins a cancellation race if it completed before deadline.
    if (request.deadline.has_value() && now_() >= *request.deadline) {
        return transport_failure(TransportFailureCategory::timeout, "transport returned after request deadline", true);
    }

    auto response = std::move(transport_result).value();
    if (response.status < 100 || response.status > 599) {
        return transport_failure(TransportFailureCategory::protocol, "transport returned an invalid HTTP status", true);
    }
    if (response.status < 200) {
        return transport_failure(TransportFailureCategory::protocol, "transport returned an interim informational response", true);
    }
    if (response.status >= 200 && response.status <= 299) {
        return RestResult::success(std::move(response));
    }

    RestError error{ std::in_place_type<DiscordApiError>, make_api_error(response) };
    return RestResult::failure(std::move(error));
}

RestResult RestClient::execute(const HttpRequest& request, const RouteDescriptor& route,
    RateLimitCoordinator& coordinator, std::stop_token cancellation) const {
    const auto as_transport_error = [](TransportError error) {
        RestError wrapped{ std::in_place_type<TransportError>, std::move(error) };
        return RestResult::failure(std::move(wrapped));
    };
    if (cancellation.stop_requested()) {
        return as_transport_error({ TransportFailureCategory::cancelled, "request cancelled before rate-limit scheduling", false });
    }
    if (request.deadline && coordinator.now() >= *request.deadline) {
        return as_transport_error({ TransportFailureCategory::timeout, "request deadline expired before rate-limit scheduling", false });
    }
    if (route.method != request.method) {
        return as_transport_error({ TransportFailureCategory::protocol, "route descriptor method does not match HTTP request", false });
    }

    auto acquired = coordinator.acquire_detailed(route, request.deadline, cancellation);
    if (acquired.state() == ResultState::error) {
        auto error = std::move(acquired).error();
        if (std::holds_alternative<TransportError>(error))
            return as_transport_error(std::move(std::get<TransportError>(error)));
        RestError metadata{ std::in_place_type<RateLimitMetadataError>, std::move(std::get<RateLimitMetadataError>(error)) };
        return RestResult::failure(std::move(metadata));
    }
    if (acquired.state() == ResultState::valueless) {
        return as_transport_error({ TransportFailureCategory::protocol, "rate-limit coordinator returned a valueless permit result", false });
    }
    auto permit = std::move(acquired).value();
    // The permit may have been granted just before a scheduling pause crossed
    // the caller's cancellation/deadline boundary. Recheck before charging it
    // as dispatched; permit destruction restores this undispatched reservation.
    if (cancellation.stop_requested())
        return as_transport_error({ TransportFailureCategory::cancelled, "request cancelled before transport dispatch", false });
    if (request.deadline && coordinator.now() >= *request.deadline)
        return as_transport_error({ TransportFailureCategory::timeout, "request deadline expired before transport dispatch", false });
    permit.mark_dispatched();
    auto transport_result = transport_.send(request, cancellation);
    if (transport_result.state() == ResultState::error) {
        const auto error = std::move(transport_result).error();
        coordinator.observe_transport_error(std::move(permit), error);
        return as_transport_error(error);
    }
    if (transport_result.state() == ResultState::valueless) {
        const TransportError error{ TransportFailureCategory::protocol, "transport returned a valueless result", true };
        coordinator.observe_transport_error(std::move(permit), error);
        return as_transport_error(error);
    }

    auto response = std::move(transport_result).value();
    auto metadata_error = coordinator.observe_response(std::move(permit), response);
    if (request.deadline && coordinator.now() >= *request.deadline) {
        return as_transport_error({ TransportFailureCategory::timeout, "transport returned after request deadline", true });
    }
    if (metadata_error) {
        metadata_error->response = make_api_error(response);
        RestError error{ std::in_place_type<RateLimitMetadataError>, std::move(*metadata_error) };
        return RestResult::failure(std::move(error));
    }
    if (response.status < 100 || response.status > 599) {
        return as_transport_error({ TransportFailureCategory::protocol, "transport returned an invalid HTTP status", true });
    }
    if (response.status < 200) {
        return as_transport_error({ TransportFailureCategory::protocol, "transport returned an interim informational response", true });
    }
    if (response.status <= 299) return RestResult::success(std::move(response));
    RestError error{ std::in_place_type<DiscordApiError>, make_api_error(response) };
    return RestResult::failure(std::move(error));
}

} // namespace discordcpp

