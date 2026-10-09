// ---- TLS 支持(跨机通信加密,2026-10 增补) ----
#include <openssl/ssl.h>
#include <openssl/err.h>

static SSL_CTX *g_tlsCtx = nullptr;
static bool g_tlsEnabled = false;

static bool tlsInit(const std::string &certPath, const std::string &keyPath) {
    if (certPath.empty() || keyPath.empty()) return false;
    SSL_library_init();
    SSL_load_error_strings();
    g_tlsCtx = SSL_CTX_new(TLS_server_method());
    if (!g_tlsCtx) return false;
    if (SSL_CTX_use_certificate_file(g_tlsCtx, certPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(g_tlsCtx); g_tlsCtx = nullptr; return false;
    }
    if (SSL_CTX_use_PrivateKey_file(g_tlsCtx, keyPath.c_str(), SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(g_tlsCtx); g_tlsCtx = nullptr; return false;
    }
    g_tlsEnabled = true;
    return true;
}

static SSL *tlsAccept(int clientFd) {
    SSL *ssl = SSL_new(g_tlsCtx);
    SSL_set_fd(ssl, clientFd);
    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(clientFd);
        return nullptr;
    }
    return ssl;
}
