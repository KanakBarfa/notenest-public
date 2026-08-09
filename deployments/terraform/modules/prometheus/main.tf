terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "prometheus" {
  name         = "prom/prometheus:v2.47.0"
  keep_locally = true
}

resource "docker_volume" "prometheus_data" {
  name = "${var.container_name}-data"
}

resource "docker_container" "prometheus" {
  name  = var.container_name
  image = docker_image.prometheus.image_id

  dynamic "ports" {
    for_each = var.host_port > 0 ? [1] : []
    content {
      internal = 9090
      external = var.host_port
    }
  }

  networks_advanced {
    name = var.network_name
  }

  volumes {
    volume_name    = docker_volume.prometheus_data.name
    container_path = "/prometheus"
  }

  volumes {
    host_path      = abspath("${path.cwd}/../../deployments/prometheus/prometheus.yml")
    container_path = "/etc/prometheus/prometheus.yml"
  }

  volumes {
    host_path      = abspath("${path.cwd}/../../deployments/prometheus/alerts.yml")
    container_path = "/etc/prometheus/alerts.yml"
  }
}
