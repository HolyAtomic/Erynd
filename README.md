# Erynd++

**Built for precision.**

Erynd++ is an independent modern C++20 library for interacting with the Discord API.

Designed with a focus on reliability, explicit behavior, type safety, and extensibility, Erynd aims to provide a solid foundation for building Discord applications in C++.

## Features

- Modern C++20 design
- Type-safe result and error handling
- HTTP transport abstraction
- REST client architecture
- Rate-limit coordination
- CMake-based build system
- Installable CMake package
- Unit testing infrastructure

## Project Status

🚧 **Early Development**

Erynd++ is actively being developed. Core infrastructure is being implemented and validated incrementally.

The current development focuses on foundational HTTP, REST, error-handling, and rate-limit components.

Not all Discord API features are implemented yet.

## Design Principles

- **Explicitness:** predictable behavior and clear interfaces.
- **Reliability:** careful handling of errors, deadlines, and rate limits.
- **Modularity:** independent components with well-defined responsibilities.
- **Extensibility:** an architecture designed to grow with the Discord API.

## Requirements

- C++20 compatible compiler
- CMake 3.24 or newer

## Building

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The core has no required HTTP implementation dependency. The optional libcurl
transport is built only when requested and requires a CMake-discoverable libcurl
7.84.0 or newer development package:

```bash
cmake -S . -B build-curl -DBUILD_TESTING=ON -DDISCORDCPP_WITH_CURL=ON
cmake --build build-curl --config Debug
ctest --test-dir build-curl -C Debug --output-on-failure
```

The Discord-specific REST API foundation is an independent optional target. It
requires an installed nlohmann/json 3.12.0 package; CMake does not download it.
Install that package and enable the target explicitly:

```bash
cmake -S . -B build-rest -DBUILD_TESTING=ON \
  -DDISCORDCPP_WITH_REST_API=ON \
  -DCMAKE_PREFIX_PATH=/path/to/dependencies
cmake --build build-rest --config Debug
```

REST API consumers link `discordcpp::rest_api`; the core remains available as
`discordcpp::discordcpp`. The API layer composes versioned origin-form paths
(for example `/api/v10/users/@me`), while the injected transport owns the
authority/origin. For `CurlHttpTransport`, set `base_url` to
`https://discord.com` when using the API layer. The existing curl default of
`https://discord.com/api/v10` remains unchanged for low-level `RestClient`
users. The REST API component borrows its `RestClient` and
`RateLimitCoordinator`; both must outlive it.

`DiscordRestClient` request construction accepts a shared `BotTokenProvider`.
The application remains responsible for secret storage; a provider supplies a
callback-scoped token view and must synchronize concurrent reads and rotation
so the view remains valid until the callback returns. The client keeps shared
ownership of the provider. Constructed requests own their `Authorization: Bot
...` header, so a transient token copy is unavoidable at that boundary;
transports may make further transient copies. No secure memory erasure is
promised. Provider exceptions are suppressed and reported as a generic local
token-unavailable error, without provider text.
The endpoint layer also redacts exact token occurrences from returned transport
diagnostics and HTTP/API error bodies before exposing those errors.

The API layer builds origin-form paths such as
`/api/v10/channels/123/messages`; the transport owns the origin.
`DiscordApiConfig::api_origin` is a validated expected origin and must match the
configured transport origin (for curl, set `base_url` to
`https://discord.com`). The default API version is 10 and the default
User-Agent identifies Erynd++; versions must be positive but are not restricted
to an allowlist. No implicit request timeout is added; pass an absolute
`steady_clock` deadline when constructing a request. `DiscordApiPathBuilder`
separates fixed route literals from encoded parameter segments and canonical
Snowflake values. Request construction does not dispatch: future endpoint
methods will continue through `RestClient` and `RateLimitCoordinator`.

`QueryParameters` is a reusable REST query builder. It accepts string,
integral, boolean, and optional values; repeated names are retained in
insertion order. Serialization uses RFC 3986 component encoding: only ASCII
unreserved bytes (`A-Z`, `a-z`, `0-9`, `-`, `.`, `_`, `~`) remain literal,
all other bytes use uppercase `%HH`, and spaces become `%20` (never `+`). Names
and values must be valid UTF-8. Empty names are rejected, and empty values
require the explicit `EmptyQueryValuePolicy::allow` option. An absent optional
value emits no pair. `DiscordApiPathBuilder::finish(query)` appends the query
without changing existing no-query path behavior. Query values are encoded as
components and cannot add raw query delimiters or change the origin/path. No
arbitrary size limit is imposed by this builder.

The synchronous REST methods include `get_current_user()`,
`get_guild(Snowflake)`, `get_channel(Snowflake)`, `get_guild_channels(Snowflake)`,
`get_guild_roles(Snowflake)`, `get_guild_emojis(Snowflake)`,
`get_guild_member(guild_id, user_id)`, `get_guild_role(guild_id, role_id)`,
`add_guild_member_role(guild_id, user_id, role_id)`,
`remove_guild_member_role(guild_id, user_id, role_id)`, and
`modify_guild_member(guild_id, user_id, changes)`. They build authenticated
requests, pass explicit normalized route metadata and typed guild/channel
major-resource IDs to the shared rate-limit coordinator, and decode model
responses where the endpoint returns one. Collection calls return vectors and fail
the whole operation if any element is malformed. They do not retry or replay;
zero resource IDs are rejected before dispatch. Guild channel listing follows
Discord's documented response (guild channels, excluding threads).

The optional REST API component also provides intentionally partial models:

- `User`: `id`, `username`, `global_name`, `avatar`, and `bot`.
- `Guild`: `id`, `name`, `icon`, and `owner_id`.
- `Channel`: `id`, `type`, `guild_id`, `name`, `topic`, and `position`.
- `Role`: `id`, `name`, `permissions`, `position`, `color`, `hoist`, `managed`,
  `mentionable`, `icon`, and `unicode_emoji`.
- `Emoji`: `id`, `name`, `roles`, `require_colons`, `managed`, `animated`, and
  `available`.
- `GuildMember`: optional nested `user`, `nick`, and guild `avatar`; required
  `roles`, nullable `joined_at`, `deaf`, and `mute`; optional `premium_since`,
  `pending`, and string `permissions`.
- `Message`: `id`, `channel_id`, `author`, `content`, `timestamp`, nullable
  `edited_timestamp`, `tts`, `mention_everyone`, `mentions`, `mention_roles`,
  `pinned`, and `type`; optional non-nullable `flags`. Message timestamps are
  retained as strings after RFC 3339 validation. The documented Message object
  has no `guild_id` field, so this model does not add one.

`GuildMemberModifyRequest` currently supports only `nick` and `roles`. Its
`Field<T>` members distinguish absent (not sent), null (sent as JSON null), and
concrete values. The encoder emits only supplied properties. The member role
assignment endpoints require `MANAGE_ROLES` and accept only Discord's documented
empty `204 No Content` response; member modification returns and decodes a
`200 OK` GuildMember. Discord permission failures remain API errors.

These are not complete Discord resource schemas. `Field<T>` distinguishes an
omitted field, explicit JSON `null`, and a concrete value. Unknown response
properties are ignored, while known fields are type-checked. `Guild.owner_id`
and `Channel.guild_id` are non-nullable when present, matching Discord's
schemas; the partial models can represent them as absent. Nullable fields keep
their null state. Snowflakes are parsed only from decimal JSON strings and
serialize internally as strings; numeric JSON IDs are rejected. JSON codecs
remain private to the implementation and are not part of the public API.
Guild collection methods use Discord's distinct collection response schemas:
[`Get Guild Channels`](https://discord.com/developers/docs/resources/guild#get-guild-channels)
returns channel objects and excludes threads, [`Get Guild Roles`](https://discord.com/developers/docs/resources/guild#get-guild-roles)
returns role objects, and [`List Guild Emojis`](https://discord.com/developers/docs/resources/emoji#list-guild-emojis)
returns emoji objects. The models remain partial; other response fields are
ignored unless explicitly represented above.

The `Message` model is partial, not a complete representation of every Discord
message feature. Attachments, embeds, components, stickers, polls, references,
interaction metadata, and other specialized nested message objects are deferred.
The library also preserves `content` as returned; Discord may provide an empty
value when the application does not have access to message content.

`CreateMessageRequest` currently supports text content only. `create_message()`
validates non-empty UTF-8 content containing at most 2,000 Unicode scalar values,
then synchronously sends `POST /channels/{channel_id}/messages` and decodes the
returned partial `Message`. For example:

```cpp
discordcpp::CreateMessageRequest request{ "Hello, Discord!" };
auto result = client.create_message(discordcpp::Snowflake{ channel_id }, request);
```

Discord requires `SEND_MESSAGES` in guild channels; the server remains the
authority for permission checks. This operation never retries or replays a POST,
and a transport timeout can occur after Discord has created the message. Embeds,
attachments, components, stickers, polls, references, and other message request
fields are not supported yet.

`get_message()` retrieves one message and returns `Result<Message,
DiscordClientError>`. Discord documents `VIEW_CHANNEL` and
`READ_MESSAGE_HISTORY` for guild channels, and additionally `CONNECT` for voice
channels. Non-success HTTP responses remain Discord API errors; transport,
rate-limit metadata, JSON, and model validation failures keep their distinct
error alternatives. The numeric 2xx status is not specified on the endpoint
reference page, so the method uses the existing RestClient 2xx handling and
validates the body with the partial Message codec.

```cpp
auto message_result = client.get_message(channel_id, message_id);
if (message_result.has_value()) {
    const auto& message = message_result.value();
    // Use message.id, message.content, and other supported fields.
}
```

Installed consumers link `discordcpp::discordcpp` for the core alone, or add
`discordcpp::curl_transport` to use `CurlHttpTransport`. The latter target
requires libcurl to be installed for the consumer as well. Curl transport URLs
are formed by appending each origin-form request target to its configured base
URL; the default base is `https://discord.com/api/v10`. Each synchronous send
uses its own curl handle, does not retry or follow redirects, verifies TLS, and
uses cooperative cancellation. Call `CurlHttpTransport::initialize()` during
single-threaded application startup before calling `send()`. Initialization
reports whether this runtime libcurl has thread-safe global initialization and
asynchronous DNS. Sends require asynchronous/threaded DNS by default because
every request has a connection timeout. Setting `require_async_dns_for_timeouts`
to false opts into best-effort timeout behavior, because a blocking system
resolver can exceed the deadline.
Call `initialize()` before any other libcurl use in the process when the
reported global-init capability is false; calling it during single-threaded
startup is the safe default. Configured timeout values must fit libcurl's
platform-specific `long` millisecond range. Far-future request deadlines use
the steady-clock callback check instead of being shortened to that range.

Caller-supplied `Host`, `Content-Length`, and `Transfer-Encoding` headers are
rejected to avoid authority or body-framing conflicts; other valid custom
headers are preserved. Response headers are bounded by defaults of 128 KiB
total across interim and final response blocks, 256 fields total, and 16 KiB
per field line. These limits can be adjusted in `CurlHttpTransportOptions`.
Response bodies remain unlimited unless `max_response_body_bytes` is set.
Each request gets a separate easy handle, which supports concurrent calls but
does not reuse connections between sends. HTTP base URLs are allowed for local
fixtures; credentials sent over non-loopback HTTP are transmitted in cleartext.

## Project Structure

```text
Erynd++
├── include/
│   └── erynd/
├── src/
├── tests/
├── examples/
└── CMakeLists.txt
```

## Independence

Erynd++ is an independently developed project. It is not a fork of another Discord C++ library.

## License

License to be determined.
