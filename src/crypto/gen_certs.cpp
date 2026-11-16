#include "crypto/gen_certs.hpp"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct EvpPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
};
struct X509Deleter {
    void operator()(X509* p) const { X509_free(p); }
};
struct X509ExtDeleter {
    void operator()(X509_EXTENSION* p) const { X509_EXTENSION_free(p); }
};
struct BignumDeleter {
    void operator()(BIGNUM* p) const { BN_free(p); }
};
struct BioDeleter {
    void operator()(BIO* p) const { BIO_free_all(p); }
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using X509ExtPtr = std::unique_ptr<X509_EXTENSION, X509ExtDeleter>;
using BignumPtr = std::unique_ptr<BIGNUM, BignumDeleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

[[noreturn]] void throw_openssl_error(const std::string& context) {
    unsigned long err = ERR_get_error();
    char buf[256] = {};
    if (err != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
        throw std::runtime_error(context + ": " + buf);
    }
    throw std::runtime_error(context);
}

void add_x509_ext(X509* cert, int nid, const std::string& value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);

    X509ExtPtr ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, const_cast<char*>(value.c_str())));
    if (!ext) {
        throw_openssl_error("Failed to create X509 extension");
    }
    if (X509_add_ext(cert, ext.get(), -1) != 1) {
        throw_openssl_error("Failed to add X509 extension");
    }
}

std::string build_san_value() {
    std::string san = "DNS:localhost,IP:127.0.0.1,IP:::1";

#if defined(__unix__) || defined(__APPLE__)
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr) {
        std::set<std::string> seen_ips = {"127.0.0.1", "::1"};
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) {
                continue;
            }
            int family = ifa->ifa_addr->sa_family;
            char addr_buf[INET6_ADDRSTRLEN] = {};
            if (family == AF_INET) {
                auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                if (inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf))) {
                    std::string ip(addr_buf);
                    if (seen_ips.insert(ip).second) {
                        san += ",IP:" + ip;
                    }
                }
            } else if (family == AF_INET6) {
                auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
                // Skip link-local IPv6 addresses (fe80::/10) because SAN IP entries cannot carry scope IDs
                if (!IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) &&
                    inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, sizeof(addr_buf))) {
                    std::string ip(addr_buf);
                    if (seen_ips.insert(ip).second) {
                        san += ",IP:" + ip;
                    }
                }
            }
        }
        freeifaddrs(ifaddr);
    }
#endif

    return san;
}

bool is_existing_cert_valid(const std::filesystem::path& cert_path,
                            const std::filesystem::path& key_path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(cert_path, ec) ||
        !std::filesystem::is_regular_file(key_path, ec)) {
        return false;
    }
    if (std::filesystem::file_size(cert_path, ec) == 0 || ec ||
        std::filesystem::file_size(key_path, ec) == 0 || ec) {
        return false;
    }

    BioPtr bio(BIO_new_file(cert_path.string().c_str(), "rb"));
    if (!bio) {
        return false;
    }
    X509Ptr cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!cert) {
        return false;
    }

    // Check that the certificate is not expired (notAfter > now)
    if (X509_cmp_current_time(X509_get0_notAfter(cert.get())) <= 0) {
        return false;
    }

    return true;
}

} // namespace

std::filesystem::path default_cert_dir() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        return std::filesystem::path(appdata) / "remote-stream";
    }
    if (const char* userprofile = std::getenv("USERPROFILE"); userprofile && *userprofile) {
        return std::filesystem::path(userprofile) / ".remote-stream";
    }
    return std::filesystem::current_path() / ".remote-stream";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "remote-stream";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "remote-stream";
    }
    return std::filesystem::current_path() / ".remote-stream";
#endif
}

void generate_self_signed_cert(const std::filesystem::path& cert_path,
                               const std::filesystem::path& key_path) {
    // 1. Generate ECDSA P-256 key pair
    EvpPkeyCtxPtr key_ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    if (!key_ctx) {
        throw_openssl_error("Failed to create EVP_PKEY_CTX");
    }
    if (EVP_PKEY_keygen_init(key_ctx.get()) <= 0) {
        throw_openssl_error("Failed to initialize keygen");
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_ctx.get(), NID_X9_62_prime256v1) <= 0) {
        throw_openssl_error("Failed to set P-256 curve");
    }

    EVP_PKEY* raw_pkey = nullptr;
    if (EVP_PKEY_keygen(key_ctx.get(), &raw_pkey) <= 0 || !raw_pkey) {
        throw_openssl_error("Failed to generate EC private key");
    }
    EvpPkeyPtr pkey(raw_pkey);

    // 2. Create X.509v3 certificate
    X509Ptr x509(X509_new());
    if (!x509) {
        throw_openssl_error("Failed to allocate X509");
    }
    if (X509_set_version(x509.get(), 2) != 1) { // version 2 == X.509 v3
        throw_openssl_error("Failed to set X509 version");
    }

    // Random 63-bit positive serial number
    BignumPtr serial_bn(BN_new());
    if (!serial_bn || BN_rand(serial_bn.get(), 63, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) {
        throw_openssl_error("Failed to generate random serial number");
    }
    if (!BN_to_ASN1_INTEGER(serial_bn.get(), X509_get_serialNumber(x509.get()))) {
        throw_openssl_error("Failed to set certificate serial number");
    }

    // Validity: now - 60s to now + 365 days
    if (!X509_gmtime_adj(X509_getm_notBefore(x509.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(x509.get()), 365L * 24 * 60 * 60)) {
        throw_openssl_error("Failed to set certificate validity period");
    }

    if (X509_set_pubkey(x509.get(), pkey.get()) != 1) {
        throw_openssl_error("Failed to set certificate public key");
    }

    // Subject & Issuer (self-signed)
    X509_NAME* name = X509_get_subject_name(x509.get());
    if (!name ||
        X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("remote-stream"), -1, -1, 0) != 1 ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("remote-stream"), -1, -1, 0) != 1) {
        throw_openssl_error("Failed to set certificate subject name");
    }
    if (X509_set_issuer_name(x509.get(), name) != 1) {
        throw_openssl_error("Failed to set certificate issuer name");
    }

    // X.509 v3 extensions
    add_x509_ext(x509.get(), NID_basic_constraints, "critical,CA:FALSE");
    add_x509_ext(x509.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment");
    add_x509_ext(x509.get(), NID_ext_key_usage, "serverAuth");
    add_x509_ext(x509.get(), NID_subject_key_identifier, "hash");
    add_x509_ext(x509.get(), NID_subject_alt_name, build_san_value());

    // Sign with SHA-256
    if (X509_sign(x509.get(), pkey.get(), EVP_sha256()) == 0) {
        throw_openssl_error("Failed to sign certificate");
    }

    // 3. Ensure parent directories exist
    if (cert_path.has_parent_path()) {
        std::filesystem::create_directories(cert_path.parent_path());
    }
    if (key_path.has_parent_path()) {
        std::filesystem::create_directories(key_path.parent_path());
    }

    // 4. Write private key PEM and restrict permissions
    {
        BioPtr key_bio(BIO_new_file(key_path.string().c_str(), "wb"));
        if (!key_bio) {
            throw_openssl_error("Failed to open key file for writing: " + key_path.string());
        }
        if (PEM_write_bio_PrivateKey(key_bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            throw_openssl_error("Failed to write private key PEM");
        }
    }
    {
        std::error_code ec;
        std::filesystem::permissions(
            key_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            ec);
    }

    // 5. Write certificate PEM
    {
        BioPtr cert_bio(BIO_new_file(cert_path.string().c_str(), "wb"));
        if (!cert_bio) {
            throw_openssl_error("Failed to open cert file for writing: " + cert_path.string());
        }
        if (PEM_write_bio_X509(cert_bio.get(), x509.get()) != 1) {
            throw_openssl_error("Failed to write certificate PEM");
        }
    }
}

TlsCertPaths ensure_tls_certs(const std::filesystem::path& dir) {
    TlsCertPaths paths{
        .cert_path = dir / "cert.pem",
        .key_path = dir / "key.pem",
    };

    if (!is_existing_cert_valid(paths.cert_path, paths.key_path)) {
        generate_self_signed_cert(paths.cert_path, paths.key_path);
    }

    return paths;
}
