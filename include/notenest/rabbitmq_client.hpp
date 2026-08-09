#ifndef NOTENEST_RABBITMQ_CLIENT_HPP
#define NOTENEST_RABBITMQ_CLIENT_HPP

#include <string>

namespace notenest {

class RabbitMQClient {
public:
    explicit RabbitMQClient(const std::string& amqp_url);

    // Send PDF export RPC request over RabbitMQ and return JSON response
    std::string requestPdfExport(const std::string& note_id, const std::string& title,
                                 const std::string& content, const std::string& user_id);

private:
    std::string host_{"rabbitmq"};
    int port_{5672};
    std::string user_{"guest"};
    std::string pass_{"guest"};
};

}  // namespace notenest

#endif  // NOTENEST_RABBITMQ_CLIENT_HPP
