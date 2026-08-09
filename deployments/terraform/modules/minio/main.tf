terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "minio" {
  name         = "minio/minio:RELEASE.2023-09-04T19-57-37Z"
  keep_locally = true
}

resource "docker_volume" "minio_data" {
  name = "${var.container_name}-data"
}

resource "docker_container" "minio" {
  name  = var.container_name
  image = docker_image.minio.image_id
  command = [
    "server",
    "/data",
    "--console-address",
    ":9001"
  ]

  env = [
    "MINIO_ROOT_USER=minioadmin",
    "MINIO_ROOT_PASSWORD=minioadmin"
  ]

  dynamic "ports" {
    for_each = var.host_port > 0 ? [1] : []
    content {
      internal = 9000
      external = var.host_port
    }
  }

  dynamic "ports" {
    for_each = var.console_port > 0 ? [1] : []
    content {
      internal = 9001
      external = var.console_port
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["minio"]
  }

  networks_advanced {
    name    = var.data_network_name
    aliases = ["minio"]
  }

  volumes {
    volume_name    = docker_volume.minio_data.name
    container_path = "/data"
  }
}
