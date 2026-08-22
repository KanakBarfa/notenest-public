terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
    random = {
      source  = "hashicorp/random"
      version = "~> 3.5"
    }
  }
}

resource "docker_image" "auth" {
  name         = "notenest-auth:latest"
  keep_locally = true
}

resource "docker_image" "user" {
  name         = "notenest-user:latest"
  keep_locally = true
}

resource "docker_image" "graphql" {
  name         = "notenest-graphql:latest"
  keep_locally = true
}

resource "docker_container" "auth" {
  name  = "${var.prefix}-auth"
  image = docker_image.auth.image_id

  env = [
    "PORT=50051",
    "DB_HOST=${var.postgres_container_name}",
    "DB_PORT=5432",
    "DB_NAME=postgres",
    "DB_USER=postgres",
    "DB_PASSWORD=pass",
    "JWT_SECRET=${var.jwt_secret}",
    "CONSUL_HOST=${var.consul_container_name}",
    "CONSUL_PORT=8500"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 50051
      external = 50051
    }
  }

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 9101
      external = 9101
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["auth-service"]
  }
}

resource "docker_container" "user" {
  name  = "${var.prefix}-user"
  image = docker_image.user.image_id

  env = [
    "PORT=50052",
    "DB_HOST=${var.postgres_container_name}",
    "DB_PORT=5432",
    "DB_NAME=postgres",
    "DB_USER=postgres",
    "DB_PASSWORD=pass",
    "CONSUL_HOST=${var.consul_container_name}",
    "CONSUL_PORT=8500"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 50052
      external = 50052
    }
  }

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 9102
      external = 9102
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["user-service"]
  }
}

resource "docker_container" "graphql" {
  name  = "${var.prefix}-graphql"
  image = docker_image.graphql.image_id

  env = [
    "PORT=4000",
    "DB_HOST=${var.postgres_container_name}",
    "DB_PORT=5432",
    "DB_NAME=postgres",
    "DB_USER=postgres",
    "DB_PASSWORD=pass",
    "JWT_SECRET=${var.jwt_secret}",
    "CONSUL_HOST=${var.consul_container_name}",
    "CONSUL_PORT=8500"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 4000
      external = 4000
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["graphql-service"]
  }
  networks_advanced {
    name    = var.app_network_name
    aliases = ["graphql"]
  }
}

resource "docker_image" "notification" {
  name         = "notenest-notification:latest"
  keep_locally = true
}

resource "docker_container" "notification" {
  name  = "${var.prefix}-notification"
  image = docker_image.notification.image_id

  env = [
    "KAFKA_BROKERS=${var.kafka_container_name}:9092",
    "NOTE_SERVICE_URL=${var.app_container_name}:50054",
    "REDIS_HOST=${var.redis_container_name}",
    "REDIS_PORT=6379",
    "CONSUL_HOST=${var.consul_container_name}",
    "CONSUL_PORT=8500"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 9103
      external = 9103
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["notification-service"]
  }
}
