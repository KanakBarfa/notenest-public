terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "app" {
  name         = "notenest-app:latest"
  keep_locally = true
}

resource "docker_image" "nginx" {
  name         = "notenest-nginx:latest"
  keep_locally = true
}

resource "docker_container" "app" {
  name  = var.container_name
  image = docker_image.app.image_id

  env = [
    "PORT=8080",
    "DB_HOST=${var.postgres_container_name}",
    "DB_PORT=5432",
    "DB_NAME=postgres",
    "DB_USER=postgres",
    "DB_PASSWORD=pass",
    "REDIS_HOST=${var.redis_container_name}",
    "REDIS_PORT=6379",
    "MINIO_ENDPOINT=http://${var.minio_container_name}:9000",
    "MINIO_ACCESS_KEY=minioadmin",
    "MINIO_SECRET_KEY=minioadmin",
    "MINIO_BUCKET=notenest-attachments",
    "MINIO_EXTERNAL_ENDPOINT=http://localhost:9000",
    "KAFKA_BROKERS=${var.kafka_container_name}:9092",
    "JWT_SECRET=${var.jwt_secret}",
    "CONSUL_HOST=notenest-dev-consul",
    "CONSUL_PORT=8500"
  ]

  dynamic "ports" {
    for_each = var.web_port > 0 ? [1] : []
    content {
      internal = 50053
      external = 50053
    }
  }

  dynamic "ports" {
    for_each = var.web_port > 0 ? [1] : []
    content {
      internal = 50054
      external = 50054
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["app"]
  }

  networks_advanced {
    name    = var.app_network_name
    aliases = ["app"]
  }

  networks_advanced {
    name = var.data_network_name
  }
}

resource "docker_container" "nginx" {
  name  = var.nginx_container_name
  image = docker_image.nginx.image_id

  dynamic "ports" {
    for_each = var.web_port > 0 ? [1] : []
    content {
      internal = 80
      external = var.web_port
    }
  }

  networks_advanced {
    name = var.network_name
  }

  networks_advanced {
    name    = var.public_network_name
    aliases = ["nginx"]
  }

  networks_advanced {
    name = var.app_network_name
  }

  depends_on = [
    docker_container.app
  ]
}
