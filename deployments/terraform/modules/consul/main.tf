terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

resource "docker_image" "consul" {
  name         = "hashicorp/consul:1.15"
  keep_locally = true
}

resource "docker_container" "consul" {
  name  = var.container_name
  image = docker_image.consul.image_id
  command = [
    "agent",
    "-dev",
    "-client=0.0.0.0"
  ]

  dynamic "ports" {
    for_each = var.host_port > 0 ? [1] : []
    content {
      internal = 8500
      external = var.host_port
    }
  }

  networks_advanced {
    name    = var.network_name
    aliases = ["consul"]
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["consul"]
  }
}
