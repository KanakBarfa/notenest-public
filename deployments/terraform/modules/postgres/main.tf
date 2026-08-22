terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "postgres" {
  name         = "postgres:15-alpine"
  keep_locally = true
}

resource "docker_volume" "postgres_data" {
  name = "${var.container_name}-data"
}

resource "docker_container" "postgres" {
  name  = var.container_name
  image = docker_image.postgres.image_id

  # wal_level=replica keeps parity with docker-compose and enables standbys.
  command = [
    "postgres",
    "-c", "wal_level=replica",
    "-c", "max_wal_senders=10",
    "-c", "max_replication_slots=10",
    "-c", "hot_standby=on"
  ]

  env = [
    "POSTGRES_USER=${var.postgres_user}",
    "POSTGRES_PASSWORD=${var.postgres_password}",
    "POSTGRES_DB=${var.postgres_db}"
  ]

  dynamic "ports" {
    for_each = var.host_port > 0 ? [1] : []
    content {
      internal = 5432
      external = var.host_port
    }
  }

  networks_advanced {
    name = var.network_name
  }

  volumes {
    volume_name    = docker_volume.postgres_data.name
    container_path = "/var/lib/postgresql/data"
  }

  volumes {
    host_path      = abspath("${path.cwd}/../../db/initdb")
    container_path = "/docker-entrypoint-initdb.d"
  }
}
