// dns_dot.cpp — DoT over Schannel (see dns_dot.h).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include "dns_dot.h"
#include "dns_frame.h"

#include <cstring>
#include <map>
#include <mutex>

namespace netshutup::dns {

namespace {

QueryResult fail(const std::wstring& why) {
    QueryResult r;
    r.ok = false;
    r.error = why;
    return r;
}

// ── One TLS connection ────────────────────────────────────────────────────────

class TlsConn {
public:
    TlsConn() : frames_(65535) {}
    ~TlsConn() { close(); }
    TlsConn(const TlsConn&) = delete;
    TlsConn& operator=(const TlsConn&) = delete;

    // Connects to ip:port and completes the TLS 1.2 handshake with `server_name`.
    bool open(const wchar_t* ip, unsigned short port, const wchar_t* server_name,
              unsigned timeout_ms, std::wstring& err) {
        close();
        if (!tcp_connect(ip, port, timeout_ms, err)) return false;
        if (!handshake(server_name, err)) { close(); return false; }
        return true;
    }

    bool is_open() const { return have_ctx_; }

    // Sends one DNS message and returns the next complete response message.
    bool exchange(const packet::Bytes& wire, packet::Bytes& response, std::wstring& err) {
        packet::Bytes framed = packet::frame_tcp(wire);
        if (framed.empty()) { err = L"query too large"; return false; }
        if (!send_encrypted(framed, err)) return false;
        frames_.reset();
        pending_plain_.clear();
        for (;;) {
            if (auto m = frames_.next()) { response = std::move(*m); return true; }
            if (frames_.broken()) { err = L"bad DNS frame from server"; return false; }
            if (!read_plaintext(err)) return false;
        }
    }

    void close() {
        if (have_ctx_) { DeleteSecurityContext(&ctx_); have_ctx_ = false; }
        if (have_cred_) { FreeCredentialsHandle(&cred_); have_cred_ = false; }
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        in_.clear();
        frames_.reset();
    }

private:
    SOCKET               sock_ = INVALID_SOCKET;
    CredHandle           cred_{};
    CtxtHandle           ctx_{};
    bool                 have_cred_ = false;
    bool                 have_ctx_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<unsigned char> in_;          // received, still encrypted
    packet::Bytes        pending_plain_;
    FrameAssembler       frames_;

    bool tcp_connect(const wchar_t* ip, unsigned short port, unsigned timeout_ms, std::wstring& err) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (InetPtonW(AF_INET, ip, &addr.sin_addr) != 1) { err = L"bad IPv4 address"; return false; }

        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) { err = L"socket failed"; return false; }

        u_long nb = 1;
        ioctlsocket(sock_, FIONBIO, &nb);
        int rc = connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { err = L"connect failed"; return false; }
        if (rc == SOCKET_ERROR) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(sock_, &wset);
            timeval tv{};
            tv.tv_sec = static_cast<long>(timeout_ms / 1000);
            tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
            if (select(0, nullptr, &wset, nullptr, &tv) != 1) { err = L"connect timed out"; return false; }
            int soerr = 0;
            int len = sizeof(soerr);
            getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
            if (soerr != 0) { err = L"connect failed"; return false; }
        }
        nb = 0;
        ioctlsocket(sock_, FIONBIO, &nb);
        DWORD t = timeout_ms;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
        return true;
    }

    bool send_all(const unsigned char* data, std::size_t len, std::wstring& err) {
        std::size_t sent = 0;
        while (sent < len) {
            int n = send(sock_, reinterpret_cast<const char*>(data + sent),
                         static_cast<int>(len - sent), 0);
            if (n <= 0) { err = L"send failed"; return false; }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Appends whatever arrives next to in_. false on timeout/close/error.
    bool recv_more(std::wstring& err) {
        unsigned char buf[16384];
        int n = recv(sock_, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
        if (n <= 0) { err = (n == 0) ? L"connection closed by server" : L"receive failed or timed out"; return false; }
        in_.insert(in_.end(), buf, buf + n);
        return true;
    }

    bool handshake(const wchar_t* server_name, std::wstring& err) {
        SCHANNEL_CRED sc{};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
        TimeStamp ts;
        SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
                                                       SECPKG_CRED_OUTBOUND, nullptr, &sc,
                                                       nullptr, nullptr, &cred_, &ts);
        if (ss != SEC_E_OK) { err = L"Schannel credentials failed (TLS 1.2 unavailable?)"; return false; }
        have_cred_ = true;

        const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                          ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        bool first = true;
        for (;;) {
            SecBuffer out_buf{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};
            SecBuffer in_bufs[2] = {};
            SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in_bufs};
            if (!first) {
                in_bufs[0] = {static_cast<unsigned long>(in_.size()), SECBUFFER_TOKEN, in_.data()};
                in_bufs[1] = {0, SECBUFFER_EMPTY, nullptr};
            }
            DWORD ret_flags = 0;
            ss = InitializeSecurityContextW(&cred_, first ? nullptr : &ctx_,
                                            const_cast<SEC_WCHAR*>(server_name), req, 0, 0,
                                            first ? nullptr : &in_desc, 0, &ctx_, &out_desc,
                                            &ret_flags, &ts);
            if (first && (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK)) have_ctx_ = true;
            first = false;

            if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                bool sent = send_all(static_cast<const unsigned char*>(out_buf.pvBuffer),
                                     out_buf.cbBuffer, err);
                FreeContextBuffer(out_buf.pvBuffer);
                if (!sent) return false;
            }

            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                if (!recv_more(err)) return false;
                continue;
            }
            if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_OK) {
                // Keep unprocessed trailing bytes, drop what Schannel consumed.
                if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0 &&
                    in_bufs[1].cbBuffer <= in_.size()) {
                    in_.erase(in_.begin(), in_.end() - in_bufs[1].cbBuffer);
                } else {
                    in_.clear();
                }
                if (ss == SEC_E_OK) break;
                if (!recv_more(err)) return false;
                continue;
            }
            err = (ss == SEC_E_UNTRUSTED_ROOT || ss == SEC_E_CERT_EXPIRED || ss == SEC_E_WRONG_PRINCIPAL ||
                   ss == static_cast<SECURITY_STATUS>(CERT_E_UNTRUSTEDROOT) ||
                   ss == static_cast<SECURITY_STATUS>(CERT_E_CN_NO_MATCH))
                      ? L"TLS certificate not accepted"
                      : L"TLS handshake failed";
            return false;
        }

        if (QueryContextAttributes(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) {
            err = L"TLS stream sizes unavailable";
            return false;
        }
        return true;
    }

    bool send_encrypted(const packet::Bytes& msg, std::wstring& err) {
        if (msg.size() > sizes_.cbMaximumMessage) { err = L"message too large for TLS record"; return false; }
        std::vector<unsigned char> buf(sizes_.cbHeader + msg.size() + sizes_.cbTrailer);
        std::memcpy(buf.data() + sizes_.cbHeader, msg.data(), msg.size());
        SecBuffer b[4];
        b[0] = {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()};
        b[1] = {static_cast<unsigned long>(msg.size()), SECBUFFER_DATA, buf.data() + sizes_.cbHeader};
        b[2] = {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sizes_.cbHeader + msg.size()};
        b[3] = {0, SECBUFFER_EMPTY, nullptr};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&ctx_, 0, &desc, 0) != SEC_E_OK) { err = L"TLS encrypt failed"; return false; }
        return send_all(buf.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer, err);
    }

    // Decrypts as many complete records as are buffered (reading more as needed)
    // and feeds plaintext to the frame assembler. Returns after at least one record.
    bool read_plaintext(std::wstring& err) {
        for (;;) {
            if (in_.empty() && !recv_more(err)) return false;
            SecBuffer b[4];
            b[0] = {static_cast<unsigned long>(in_.size()), SECBUFFER_DATA, in_.data()};
            b[1] = {0, SECBUFFER_EMPTY, nullptr};
            b[2] = {0, SECBUFFER_EMPTY, nullptr};
            b[3] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
            SECURITY_STATUS ss = DecryptMessage(&ctx_, &desc, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                if (!recv_more(err)) return false;
                continue;
            }
            if (ss != SEC_E_OK) {
                err = (ss == SEC_I_CONTEXT_EXPIRED) ? L"server closed the TLS session" : L"TLS decrypt failed";
                return false;
            }
            const unsigned char* data = nullptr;
            std::size_t data_len = 0;
            std::size_t extra = 0;
            for (int i = 0; i < 4; ++i) {
                if (b[i].BufferType == SECBUFFER_DATA) {
                    data = static_cast<const unsigned char*>(b[i].pvBuffer);
                    data_len = b[i].cbBuffer;
                } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                    extra = b[i].cbBuffer;
                }
            }
            if (data && data_len > 0 && !frames_.push(data, data_len)) { err = L"bad DNS frame from server"; return false; }
            if (extra > 0 && extra <= in_.size()) {
                in_.erase(in_.begin(), in_.end() - static_cast<std::ptrdiff_t>(extra));
            } else {
                in_.clear();
            }
            return true;
        }
    }
};

} // namespace

// ── DotTransport ──────────────────────────────────────────────────────────────

struct DotTransport::Impl {
    bool                                      wsa_ok = false;
    std::mutex                                mu;
    std::map<std::wstring, std::unique_ptr<TlsConn>> conns;   // by provider id
};

DotTransport::DotTransport() : impl_(new Impl) {
    WSADATA wsa;
    impl_->wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
}

DotTransport::~DotTransport() {
    impl_->conns.clear();   // close connections before WSACleanup
    if (impl_->wsa_ok) WSACleanup();
}

QueryResult DotTransport::query(const Upstream& upstream, const packet::Bytes& wire,
                                unsigned timeout_ms) {
    if (upstream.method != Method::DoT) return fail(L"DotTransport handles DoT only");
    const Provider* prov = find_provider(upstream.provider_id);
    if (!prov) return fail(L"unknown provider");
    if (wire.size() < 12 || wire.size() > 65535) return fail(L"query size out of range");
    if (!impl_->wsa_ok) return fail(L"Winsock could not be started");

    std::lock_guard<std::mutex> lock(impl_->mu);
    auto& slot = impl_->conns[prov->id];
    if (!slot) slot.reset(new TlsConn);

    std::wstring last_err = L"no attempt made";
    // Attempt 1 reuses an open connection; attempt 2 reconnects (stale idle connection).
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!slot->is_open() || attempt == 1) {
            slot->close();
            bool opened = slot->open(prov->ipv4_primary, prov->dot_port, prov->dot_host, timeout_ms, last_err);
            if (!opened) opened = slot->open(prov->ipv4_secondary, prov->dot_port, prov->dot_host, timeout_ms, last_err);
            if (!opened) return fail(last_err);
        }
        QueryResult r;
        if (slot->exchange(wire, r.response, last_err)) {
            r.ok = true;
            if (r.response.size() < 12) { slot->close(); return fail(L"response too short"); }
            return r;
        }
        slot->close();
    }
    return fail(last_err);
}

QueryResult DotTransport::query_legacy(const std::vector<std::wstring>&,
                                       const packet::Bytes&, unsigned) {
    return fail(L"plain DNS is not handled by DotTransport");
}

} // namespace netshutup::dns
