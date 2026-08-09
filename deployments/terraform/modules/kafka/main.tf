terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "kafka" {
  name         = "confluentinc/cp-kafka:7.5.0"
  keep_locally = true
}

resource "docker_container" "kafka" {
  name  = var.container_name
  image = docker_image.kafka.image_id

  env = [
    "KAFKA_NODE_ID=1",
    "KAFKA_LISTENER_SECURITY_PROTOCOL_MAP=CONTROLLER:PLAINTEXT,PLAINTEXT:PLAINTEXT,PLAINTEXT_HOST:PLAINTEXT",
    "KAFKA_ADVERTISED_LISTENERS=PLAINTEXT://${var.container_name}:29092,PLAINTEXT_HOST://localhost:${var.host_port}",
    "KAFKA_OFFSETS_TOPIC_REPLICATION_FACTOR=1",
    "KAFKA_GROUP_INITIAL_REBALANCE_DELAY_MS=0",
    "KAFKA_TRANSACTION_STATE_LOG_MIN_ISR=1",
    "KAFKA_TRANSACTION_STATE_LOG_REPLICATION_FACTOR=1",
    "KAFKA_PROCESS_ROLES=broker,controller",
    "KAFKA_CONTROLLER_QUORUM_VOTERS=1@${var.container_name}:29093",
    "KAFKA_LISTENERS=PLAINTEXT://0.0.0.0:29092,CONTROLLER://0.0.0.0:29093,PLAINTEXT_HOST://0.0.0.0:${var.host_port}",
    "KAFKA_INTER_BROKER_LISTENER_NAME=PLAINTEXT",
    "KAFKA_CONTROLLER_LISTENER_NAMES=CONTROLLER",
    "KAFKA_LOG_DIRS=/tmp/kraft-combined-logs",
    "CLUSTER_ID=MkU3OEVBNTcwNTJENDM2Qk"
  ]

  dynamic "ports" {
    for_each = var.host_port > 0 ? [1] : []
    content {
      internal = var.host_port
      external = var.host_port
    }
  }

  networks_advanced {
    name = var.network_name
  }
}
