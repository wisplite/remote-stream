#pragma once

#include <filesystem>

struct TlsCertPaths {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;
};

// Returns the persistent directory in the user's home/config folder
// where TLS certificates are stored (e.g. ~/.config/remote-stream on Linux).
std::filesystem::path default_cert_dir();

// Generates a self-signed X.509v3 TLS certificate (ECDSA P-256, SHA-256)
// and writes the PEM-encoded certificate and private key to cert_path and key_path.
void generate_self_signed_cert(const std::filesystem::path& cert_path,
                               const std::filesystem::path& key_path);

// Ensures a valid self-signed certificate and key exist in `dir`.
// Reuses existing files if present and non-empty; otherwise generates new ones.
TlsCertPaths ensure_tls_certs(const std::filesystem::path& dir = default_cert_dir());
