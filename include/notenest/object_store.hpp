#ifndef OBJECT_STORE_HPP
#define OBJECT_STORE_HPP

#include <memory>
#include <notenest/circuit_breaker.hpp>
#include <string>

// Interface for Object Storage.
class ObjectStore {
public:
    virtual ~ObjectStore() = default;

    // Generates a presigned PUT URL for uploading attachments.
    virtual std::string generatePresignedPutUrl(const std::string& bucket, const std::string& key,
                                                int expiry_seconds) = 0;

    // Generates a presigned GET URL for downloading/rendering attachments.
    virtual std::string generatePresignedGetUrl(const std::string& bucket, const std::string& key,
                                                int expiry_seconds) = 0;

    // Deletes an object from storage.
    virtual bool deleteObject(const std::string& bucket, const std::string& key) = 0;

    // Verifies or creates the bucket in the object store.
    virtual bool createBucket(const std::string& bucket) = 0;
};

// S3-compatible implementation of ObjectStore with CircuitBreaker.
class S3ObjectStore : public ObjectStore {
public:
    S3ObjectStore(const std::string& endpoint, const std::string& access_key,
                  const std::string& secret_key, const std::string& external_endpoint);

    std::string generatePresignedPutUrl(const std::string& bucket, const std::string& key,
                                        int expiry_seconds) override;
    std::string generatePresignedGetUrl(const std::string& bucket, const std::string& key,
                                        int expiry_seconds) override;
    bool deleteObject(const std::string& bucket, const std::string& key) override;
    bool createBucket(const std::string& bucket) override;

    // Returns circuit breaker state name.
    std::string getCircuitState() const;

private:
    std::string endpoint_;
    std::string access_key_;
    std::string secret_key_;
    std::string external_endpoint_;
    CircuitBreaker circuit_breaker_;
};

#endif
