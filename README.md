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
development package:

```bash
cmake -S . -B build-curl -DBUILD_TESTING=ON -DDISCORDCPP_WITH_CURL=ON
cmake --build build-curl --config Debug
ctest --test-dir build-curl -C Debug --output-on-failure
```

Installed consumers link `discordcpp::discordcpp` for the core alone, or add
`discordcpp::curl_transport` to use `CurlHttpTransport`. The latter target
requires libcurl to be installed for the consumer as well. Curl transport URLs
are formed by appending each origin-form request target to its configured base
URL; the default base is `https://discord.com/api/v10`. Each synchronous send
uses its own curl handle, does not retry or follow redirects, verifies TLS, and
honors request deadlines with cooperative cancellation. Configure a response
body limit when responses are untrusted or may be large; the default is no cap.

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
