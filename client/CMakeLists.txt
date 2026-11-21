# zproject_client: what zproject-cli (and later the desktop app) needs to
# talk to zprojectd (SPEC.md §10, §16.5): settings, the token store
# (macOS Keychain, libsecret's secret-tool, or a 0600 file), a small
# synchronous /v1 client and the CLI commands. Qt Core + Network only.
add_library(zproject_client STATIC
  ApiClient.cpp ApiClient.hpp
  Cli.cpp Cli.hpp
  ClientConfig.cpp ClientConfig.hpp
  TokenStore.cpp TokenStore.hpp)
target_link_libraries(zproject_client PUBLIC zproject_core Qt6::Network)
zproject_set_warnings(zproject_client)
