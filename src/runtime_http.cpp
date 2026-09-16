#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <algorithm>
#include <curl/curl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" void* quidra_managed_alloc(unsigned long long bytes);
extern "C" void quidra_managed_release(void* value, void* drop_function);
extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long size);

namespace {
char* http_copy_string(const std::string& value) {
    return quidra_runtime_copy_text(
        value.data(), static_cast<unsigned long long>(value.size()));
}
}

namespace {
struct HttpHeaderStore {
    std::vector<std::pair<std::string, std::string>> entries;
};

struct HttpCapture {
    std::vector<unsigned char> body;
    HttpHeaderStore headers;
    std::string callback_error;
};

thread_local std::string http_last_error;
std::once_flag http_curl_once;
CURLcode http_curl_init_result = CURLE_FAILED_INIT;

std::string http_ascii_lower(std::string value) {
    for (char& c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') c = static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

std::string http_trim(std::string value) {
    std::size_t first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t' ||
            value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           (value[last - 1] == ' ' || value[last - 1] == '\t' ||
            value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

size_t http_body_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* capture = static_cast<HttpCapture*>(userdata);
    if (!capture || !data) return 0;
    if (size != 0 && count > static_cast<size_t>(-1) / size) {
        capture->callback_error = "HTTP response body size overflow";
        return 0;
    }
    const std::size_t bytes = size * count;
    try {
        capture->body.insert(
            capture->body.end(),
            reinterpret_cast<unsigned char*>(data),
            reinterpret_cast<unsigned char*>(data) + bytes);
    } catch (...) {
        capture->callback_error = "cannot allocate HTTP response body";
        return 0;
    }
    return bytes;
}

size_t http_header_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* capture = static_cast<HttpCapture*>(userdata);
    if (!capture || !data) return 0;
    if (size != 0 && count > static_cast<size_t>(-1) / size) {
        capture->callback_error = "HTTP response header size overflow";
        return 0;
    }
    const std::size_t bytes = size * count;
    try {
        std::string line(data, bytes);
        line = http_trim(std::move(line));
        const auto separator = line.find(':');
        if (separator != std::string::npos) {
            auto name = http_ascii_lower(http_trim(line.substr(0, separator)));
            auto value = http_trim(line.substr(separator + 1));
            if (!name.empty()) capture->headers.entries.emplace_back(std::move(name), std::move(value));
        }
    } catch (...) {
        capture->callback_error = "cannot allocate HTTP response headers";
        return 0;
    }
    return bytes;
}

void http_set_error(const std::string& message) {
    http_last_error = message.empty() ? "HTTP request failed" : message;
}

void* http_make_bytes(const std::vector<unsigned char>& body) {
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max())) return nullptr;
    if (body.size() > static_cast<std::size_t>(-1) - 8) return nullptr;
    auto* value = static_cast<unsigned char*>(quidra_managed_alloc(8 + body.size()));
    const auto length = static_cast<long long>(body.size());
    std::memcpy(value, &length, sizeof(length));
    if (!body.empty()) std::memcpy(value + 8, body.data(), body.size());
    return value;
}

void* http_make_response(long status, HttpCapture&& capture) {
    auto* body = http_make_bytes(capture.body);
    if (!body) return nullptr;

    HttpHeaderStore* headers = nullptr;
    try {
        headers = new HttpHeaderStore(std::move(capture.headers));
    } catch (...) {
        quidra_managed_release(body, nullptr);
        return nullptr;
    }

    auto* response = static_cast<unsigned char*>(quidra_managed_alloc(24));
    const long long status_value = static_cast<long long>(status);
    const auto header_bits = reinterpret_cast<std::uintptr_t>(headers);
    std::memcpy(response, &status_value, 8);
    std::memcpy(response + 8, &body, sizeof(body));
    std::memcpy(response + 16, &header_bits, sizeof(header_bits));
    return response;
}

HttpHeaderStore* http_headers_from_response(void* response) {
    if (!response) return nullptr;
    std::uintptr_t bits = 0;
    std::memcpy(&bits, static_cast<unsigned char*>(response) + 16, sizeof(bits));
    return reinterpret_cast<HttpHeaderStore*>(bits);
}

void* http_clone_bytes(void* source) {
    if (!source) return nullptr;
    long long length = 0;
    std::memcpy(&length, source, sizeof(length));
    if (length < 0) return nullptr;
    const auto size = static_cast<unsigned long long>(length);
    if (size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()) - 8ULL) {
        return nullptr;
    }
    auto* copy = static_cast<unsigned char*>(quidra_managed_alloc(8ULL + size));
    std::memcpy(copy, source, static_cast<std::size_t>(8ULL + size));
    return copy;
}
}

extern "C" void* quidra_http_response_clone(void* response) {
    if (!response) return nullptr;

    long long status = 0;
    void* body = nullptr;
    std::uintptr_t header_bits = 0;
    std::memcpy(&status, response, sizeof(status));
    std::memcpy(&body, static_cast<unsigned char*>(response) + 8, sizeof(body));
    std::memcpy(&header_bits, static_cast<unsigned char*>(response) + 16, sizeof(header_bits));

    auto* body_copy = http_clone_bytes(body);
    if (body && !body_copy) return nullptr;

    HttpHeaderStore* headers_copy = nullptr;
    try {
        const auto* headers = reinterpret_cast<HttpHeaderStore*>(header_bits);
        headers_copy = headers ? new HttpHeaderStore(*headers) : new HttpHeaderStore;
    } catch (...) {
        quidra_managed_release(body_copy, nullptr);
        return nullptr;
    }

    auto* copy = static_cast<unsigned char*>(quidra_managed_alloc(24));
    const auto copied_header_bits = reinterpret_cast<std::uintptr_t>(headers_copy);
    std::memcpy(copy, &status, sizeof(status));
    std::memcpy(copy + 8, &body_copy, sizeof(body_copy));
    std::memcpy(copy + 16, &copied_header_bits, sizeof(copied_header_bits));
    return copy;
}

extern "C" void quidra_http_response_drop(void* response) {
    if (!response) return;
    void* body = nullptr;
    std::uintptr_t header_bits = 0;
    std::memcpy(&body, static_cast<unsigned char*>(response) + 8, sizeof(body));
    std::memcpy(&header_bits, static_cast<unsigned char*>(response) + 16, sizeof(header_bits));
    quidra_managed_release(body, nullptr);
    delete reinterpret_cast<HttpHeaderStore*>(header_bits);

    void* no_body = nullptr;
    std::uintptr_t no_headers = 0;
    std::memcpy(static_cast<unsigned char*>(response) + 8, &no_body, sizeof(no_body));
    std::memcpy(static_cast<unsigned char*>(response) + 16, &no_headers, sizeof(no_headers));
}

extern "C" void* quidra_http_get(const char* url) {
    http_last_error.clear();
    if (!url || !*url) {
        http_set_error("HTTP URL is empty");
        return nullptr;
    }

    std::call_once(http_curl_once, [] {
        http_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (http_curl_init_result != CURLE_OK) {
        http_set_error("cannot initialize HTTP/TLS runtime");
        return nullptr;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        http_set_error("cannot create HTTP request");
        return nullptr;
    }

    HttpCapture capture;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Quidra/0.2.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_body_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &capture);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &capture);

    const CURLcode performed = curl_easy_perform(curl);
    if (performed != CURLE_OK) {
        const std::string message = !capture.callback_error.empty()
            ? capture.callback_error
            : std::string(curl_easy_strerror(performed));
        curl_easy_cleanup(curl);
        http_set_error(message);
        return nullptr;
    }

    long status = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
        curl_easy_cleanup(curl);
        http_set_error("cannot read HTTP response status");
        return nullptr;
    }
    curl_easy_cleanup(curl);

    auto* response = http_make_response(status, std::move(capture));
    if (!response) {
        http_set_error("cannot allocate HTTP response");
        return nullptr;
    }
    return response;
}

extern "C" char* quidra_http_last_error_copy() {
    return http_copy_string(http_last_error.empty() ? "HTTP request failed" : http_last_error);
}

extern "C" char* quidra_http_header(void* response, const char* name) {
    if (!name) return nullptr;
    auto* headers = http_headers_from_response(response);
    if (!headers) return nullptr;
    const auto wanted = http_ascii_lower(http_trim(name));
    for (const auto& [header_name, header_value] : headers->entries) {
        if (header_name == wanted) return http_copy_string(header_value);
    }
    return nullptr;
}
