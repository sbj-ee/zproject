# zproject_client: what zproject-cli and the desktop app need to talk to
# zprojectd (SPEC.md §4.1, §10, §16.5): settings, the token store (macOS
# Keychain, libsecret's secret-tool, or a 0600 file) shared by both, the
# synchronous /v1 client and the CLI commands, and for the desktop the
# non-blocking client, error mapping, the read-only cache and the change-feed
# syncer. Qt Core + Network only (no GUI), so the server-only build has it.
add_library(zproject_client STATIC
  ApiClient.cpp ApiClient.hpp
  ApiError.cpp ApiError.hpp
  AsyncApiClient.cpp AsyncApiClient.hpp
  Cache.cpp Cache.hpp
  ChangeFeedSyncer.cpp ChangeFeedSyncer.hpp
  ChangeRecord.cpp ChangeRecord.hpp
  Cli.cpp Cli.hpp
  ClientConfig.cpp ClientConfig.hpp
  DesktopSettings.cpp DesktopSettings.hpp
  TokenStore.cpp TokenStore.hpp)
target_link_libraries(zproject_client PUBLIC zproject_core zproject_store Qt6::Network)
zproject_set_warnings(zproject_client)
