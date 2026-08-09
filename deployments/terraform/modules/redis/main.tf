terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "redis" {
  name         = "redis:7-alpine"
  keep_locally = true
}

resource "docker_volume" "redis_data" {
  name = "${var.container_name}-data"
}

resource "docker_container" "redis" {
  name  = var.container_name
  image = docker_image.redis.image_id

  dynamic "ports" {
    for_each = var.host_port > 0 ? [1] : []
    content {
      internal = 6379
      external = var.host_port
    }
  }

  networks_advanced {
    name = var.network_name
  }

  networks_advanced {
    name    = var.data_network_name
    aliases = ["redis"]
  }

  volumes {
    volume_name    = docker_volume.redis_data.name
    container_path = "/data"
  }
}
