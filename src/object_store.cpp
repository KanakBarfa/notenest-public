#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <notenest/crypto.hpp>
#include <notenest/object_store.hpp>
#include <notenest/utils.hpp>
#include <print>
#include <sstream>

// Helper: Get UTC date (YYYYMMDD) and timestamp (YYYYMMDDTHHMMSSZ).
static std::pair<std::string, std::string> getUTCTimes() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm;
    gmtime_r(&time, &utc_tm);

    char date_buf[16];
    char time_buf[24];
    std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &utc_tm);
    std::strftime(time_buf, sizeof(time_buf), "%Y%m%dT%H%M%SZ", &utc_tm);

    return {std::string(date_buf), std::string(time_buf)};
}

// Helper: Extract host from endpoint (stripping protocol and path).
static std::string getHostFromEndpoint(const std::string& endpoint) {
    std::string host = endpoint;
    size_t proto_pos = host.find("://");
    if (proto_pos != std::string::npos) {
        host = host.substr(proto_pos + 3);
    }
    size_t slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        host = host.substr(0, slash_pos);
    }
    return host;
}

// Helper: Parse endpoint into Host and Port.
static bool parseEndpoint(const std::string& endpoint, std::string& host, std::string& port) {
    std::string ep = endpoint;
    size_t proto_pos = ep.find("://");
    if (proto_pos != std::string::npos) {
        ep = ep.substr(proto_pos + 3);
    }
    size_t colon_pos = ep.find(':');
    size_t slash_pos = ep.find('/');
    if (colon_pos != std::string::npos &&
        (slash_pos == std::string::npos || colon_pos < slash_pos)) {
        host = ep.substr(0, colon_pos);
        if (slash_pos != std::string::npos) {
            port = ep.substr(colon_pos + 1, slash_pos - colon_pos - 1);
        } else {
            port = ep.substr(colon_pos + 1);
        }
    } else {
        if (slash_pos != std::string::npos) {
            host = ep.substr(0, slash_pos);
        } else {
            host = ep;
        }
        port = "80";
    }
    return !host.empty();
}

// Helper: Generic signed S3 raw TCP socket request sender.
static bool sendSignedS3Request(const std::string& endpoint, const std::string& method,
                                const std::string& bucket, const std::string& key,
                                const std::string& query, const std::string& body,
                                const std::string& access_key, const std::string& secret_key) {
    auto [date, timestamp] = getUTCTimes();
    std::string region = "us-east-1";
    std::string credential = access_key + "/" + date + "/" + region + "/s3/aws4_request";
    std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";

    std::string canonical_uri = "/";
    if (!bucket.empty()) {
        canonical_uri = Utils::s3UriEncode("/" + bucket, false);
        if (!key.empty()) {
            canonical_uri = Utils::s3UriEncode("/" + bucket + "/" + key, false);
        }
    }

    std::string canonical_query = query;
    std::string internal_host = getHostFromEndpoint(endpoint);
    std::string hashed_payload = Utils::sha256_hex(body);

    std::string canonical_headers = "host:" + internal_host + "\n" +
                                    "x-amz-content-sha256:" + hashed_payload + "\n" +
                                    "x-amz-date:" + timestamp + "\n";

    std::string canonical_request = method + "\n" + canonical_uri + "\n" + canonical_query + "\n" +
                                    canonical_headers + "\n" + signed_headers + "\n" +
                                    hashed_payload;

    std::string scope = date + "/" + region + "/s3/aws4_request";
    std::string string_to_sign = "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" +
                                 Utils::sha256_hex(canonical_request);

    std::string kDate = Crypto::hmac_sha256(date, "AWS4" + secret_key);
    std::string kRegion = Crypto::hmac_sha256(region, kDate);
    std::string kService = Crypto::hmac_sha256("s3", kRegion);
    std::string kSigning = Crypto::hmac_sha256("aws4_request", kService);
    std::string signature = Utils::to_hex(Crypto::hmac_sha256(string_to_sign, kSigning));

    std::string auth_header = "AWS4-HMAC-SHA256 Credential=" + credential +
                              ", SignedHeaders=" + signed_headers + ", Signature=" + signature;

    std::string host_name, port_str;
    if (!parseEndpoint(endpoint, host_name, port_str)) {
        std::println(stderr, "Failed to parse S3 endpoint: {}", endpoint);
        return false;
    }

    struct addrinfo hints, *res;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host_name.c_str(), port_str.c_str(), &hints, &res) != 0) {
        std::println(stderr, "Failed to resolve S3 host: {}", host_name);
        return false;
    }

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        freeaddrinfo(res);
        std::println(stderr, "Failed to create socket for S3 request");
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        close(sockfd);
        freeaddrinfo(res);
        std::println(stderr, "Failed to connect to S3 host: {}:{}", host_name, port_str);
        return false;
    }
    freeaddrinfo(res);

    std::string path = canonical_uri;
    if (!query.empty()) {
        path += "?" + query;
    }

    std::string request =
        method + " " + path + " HTTP/1.1\r\n" + "Host: " + host_name + ":" + port_str + "\r\n" +
        "User-Agent: NoteNest\r\n" + "X-Amz-Content-Sha256: " + hashed_payload + "\r\n" +
        "X-Amz-Date: " + timestamp + "\r\n" + "Authorization: " + auth_header + "\r\n";

    if (!body.empty()) {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    if (!body.empty()) {
        request += body;
    }

    ssize_t sent = write(sockfd, request.data(), request.size());
    if (sent < 0) {
        close(sockfd);
        std::println(stderr, "Failed to write to S3 socket");
        return false;
    }

    std::string response;
    char buf[1024];
    ssize_t n;
    while ((n = read(sockfd, buf, sizeof(buf))) > 0) {
        response.append(buf, n);
    }
    close(sockfd);

    if (response.find("HTTP/1.1 200") != std::string::npos ||
        response.find("HTTP/1.1 204") != std::string::npos ||
        response.find("HTTP/1.1 409") != std::string::npos) {
        return true;
    }
    std::println(stderr, "S3 request failed: {}", response);
    return false;
}

S3ObjectStore::S3ObjectStore(const std::string& endpoint, const std::string& access_key,
                             const std::string& secret_key, const std::string& external_endpoint)
    : endpoint_(endpoint),
      access_key_(access_key),
      secret_key_(secret_key),
      external_endpoint_(external_endpoint),
      circuit_breaker_(5, std::chrono::milliseconds(5000)) {}

std::string S3ObjectStore::generatePresignedPutUrl(const std::string& bucket,
                                                   const std::string& key, int expiry_seconds) {
    auto [date, timestamp] = getUTCTimes();
    std::string region = "us-east-1";
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credential = access_key_ + "/" + date + "/" + region + "/s3/aws4_request";
    std::string signed_headers = "host";

    std::string canonical_uri = Utils::s3UriEncode("/" + bucket + "/" + key, false);
    std::string canonical_query =
        "X-Amz-Algorithm=" + algorithm + "&X-Amz-Credential=" + Utils::urlEncode(credential) +
        "&X-Amz-Date=" + timestamp + "&X-Amz-Expires=" + std::to_string(expiry_seconds) +
        "&X-Amz-SignedHeaders=" + signed_headers;

    std::string host_val = getHostFromEndpoint(external_endpoint_);
    std::string canonical_headers = "host:" + host_val + "\n";

    std::string canonical_request = "PUT\n" + canonical_uri + "\n" + canonical_query + "\n" +
                                    canonical_headers + "\n" + signed_headers + "\n" +
                                    "UNSIGNED-PAYLOAD";

    std::string scope = date + "/" + region + "/s3/aws4_request";
    std::string string_to_sign = "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" +
                                 Utils::sha256_hex(canonical_request);

    std::string kDate = Crypto::hmac_sha256(date, "AWS4" + secret_key_);
    std::string kRegion = Crypto::hmac_sha256(region, kDate);
    std::string kService = Crypto::hmac_sha256("s3", kRegion);
    std::string kSigning = Crypto::hmac_sha256("aws4_request", kService);
    std::string signature = Utils::to_hex(Crypto::hmac_sha256(string_to_sign, kSigning));

    std::string ext_ep = external_endpoint_;
    if (!ext_ep.empty() && ext_ep.back() == '/') {
        ext_ep.pop_back();
    }

    return ext_ep + canonical_uri + "?" + canonical_query + "&X-Amz-Signature=" + signature;
}

std::string S3ObjectStore::generatePresignedGetUrl(const std::string& bucket,
                                                   const std::string& key, int expiry_seconds) {
    auto [date, timestamp] = getUTCTimes();
    std::string region = "us-east-1";
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credential = access_key_ + "/" + date + "/" + region + "/s3/aws4_request";
    std::string signed_headers = "host";

    std::string canonical_uri = Utils::s3UriEncode("/" + bucket + "/" + key, false);
    std::string canonical_query =
        "X-Amz-Algorithm=" + algorithm + "&X-Amz-Credential=" + Utils::urlEncode(credential) +
        "&X-Amz-Date=" + timestamp + "&X-Amz-Expires=" + std::to_string(expiry_seconds) +
        "&X-Amz-SignedHeaders=" + signed_headers;

    std::string host_val = getHostFromEndpoint(external_endpoint_);
    std::string canonical_headers = "host:" + host_val + "\n";

    std::string canonical_request = "GET\n" + canonical_uri + "\n" + canonical_query + "\n" +
                                    canonical_headers + "\n" + signed_headers + "\n" +
                                    "UNSIGNED-PAYLOAD";

    std::string scope = date + "/" + region + "/s3/aws4_request";
    std::string string_to_sign = "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" +
                                 Utils::sha256_hex(canonical_request);

    std::string kDate = Crypto::hmac_sha256(date, "AWS4" + secret_key_);
    std::string kRegion = Crypto::hmac_sha256(region, kDate);
    std::string kService = Crypto::hmac_sha256("s3", kRegion);
    std::string kSigning = Crypto::hmac_sha256("aws4_request", kService);
    std::string signature = Utils::to_hex(Crypto::hmac_sha256(string_to_sign, kSigning));

    std::string ext_ep = external_endpoint_;
    if (!ext_ep.empty() && ext_ep.back() == '/') {
        ext_ep.pop_back();
    }

    return ext_ep + canonical_uri + "?" + canonical_query + "&X-Amz-Signature=" + signature;
}

bool S3ObjectStore::deleteObject(const std::string& bucket, const std::string& key) {
    if (!circuit_breaker_.allowRequest()) {
        std::println(stderr, "[S3ObjectStore] Circuit breaker is OPEN. Fast failing delete.");
        return false;
    }
    bool ok =
        sendSignedS3Request(endpoint_, "DELETE", bucket, key, "", "", access_key_, secret_key_);
    if (ok) {
        circuit_breaker_.recordSuccess();
    } else {
        circuit_breaker_.recordFailure();
    }
    return ok;
}

bool S3ObjectStore::createBucket(const std::string& bucket) {
    if (!circuit_breaker_.allowRequest()) {
        std::println(stderr, "[S3ObjectStore] Circuit breaker is OPEN. Fast failing createBucket.");
        return false;
    }
    bool ok = sendSignedS3Request(endpoint_, "PUT", bucket, "", "", "", access_key_, secret_key_);
    if (ok) {
        circuit_breaker_.recordSuccess();
    } else {
        circuit_breaker_.recordFailure();
    }
    return ok;
}

std::string S3ObjectStore::getCircuitState() const {
    return circuit_breaker_.getStateName();
}
