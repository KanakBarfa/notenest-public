terraform {
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
  }
}

# PgBouncer Connection Pooler
resource "docker_image" "pgbouncer" {
  name         = "edoburu/pgbouncer:latest"
  keep_locally = true
}

resource "docker_container" "pgbouncer" {
  name  = var.is_test ? "${var.prefix}-pgbouncer" : "notenest-pgbouncer-container"
  image = docker_image.pgbouncer.image_id

  env = [
    "DB_HOST=${var.postgres_container_name}",
    "DB_PORT=5432",
    "DB_NAME=postgres",
    "DB_USER=postgres",
    "DB_PASSWORD=pass",
    "POOL_MODE=transaction",
    "MAX_CLIENT_CONN=1000",
    "DEFAULT_POOL_SIZE=20",
    "LISTEN_PORT=6432",
    "AUTH_TYPE=plain"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 6432
      external = 6432
    }
  }

  networks_advanced {
    name    = var.data_network_name
    aliases = ["pgbouncer"]
  }
}

# Origin Nginx CDN
resource "docker_image" "origin_nginx" {
  name         = "nginx:alpine"
  keep_locally = true
}

resource "docker_container" "origin_nginx" {
  name  = var.is_test ? "${var.prefix}-origin-nginx" : "notenest-origin-nginx-container"
  image = docker_image.origin_nginx.image_id

  networks_advanced {
    name    = var.data_network_name
    aliases = ["origin-nginx"]
  }
}

# Edge Nginx CDN
resource "docker_image" "edge_nginx" {
  name         = "nginx:alpine"
  keep_locally = true
}

resource "docker_container" "edge_nginx" {
  name  = var.is_test ? "${var.prefix}-edge-nginx" : "notenest-edge-nginx-container"
  image = docker_image.edge_nginx.image_id

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 8081
      external = 8081
    }
  }

  networks_advanced {
    name    = var.public_network_name
    aliases = ["edge-nginx"]
  }
  networks_advanced {
    name = var.data_network_name
  }

  depends_on = [
    docker_container.origin_nginx
  ]
}

# Kong API Gateway
resource "docker_image" "kong" {
  name         = "kong:3.5"
  keep_locally = true
}

resource "docker_container" "kong" {
  name  = var.is_test ? "${var.prefix}-kong" : "notenest-kong-container"
  image = docker_image.kong.image_id

  env = [
    "KONG_DATABASE=off",
    "KONG_DECLARATIVE_CONFIG=/kong/declarative/kong.yml",
    "KONG_PROXY_LISTEN=0.0.0.0:8000",
    "KONG_LOG_LEVEL=notice"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 8000
      external = 8000
    }
  }

  volumes {
    host_path      = abspath("${path.module}/../../../kong.yml")
    container_path = "/kong/declarative/kong.yml"
  }

  networks_advanced {
    name    = var.public_network_name
    aliases = ["kong"]
  }
  networks_advanced {
    name    = var.app_network_name
    aliases = ["kong"]
  }
}

# HAProxy Load Balancer
resource "docker_image" "haproxy" {
  name         = "haproxy:2.8-alpine"
  keep_locally = true
}

resource "docker_container" "haproxy" {
  name  = var.is_test ? "${var.prefix}-haproxy" : "notenest-haproxy-container"
  image = docker_image.haproxy.image_id

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 80
      external = 80
    }
  }

  volumes {
    host_path      = abspath("${path.module}/../../../haproxy.cfg")
    container_path = "/usr/local/etc/haproxy/haproxy.cfg"
  }

  networks_advanced {
    name    = var.public_network_name
    aliases = ["haproxy"]
  }
}

# Observability Containers
resource "docker_container" "prometheus_full" {
  name  = var.is_test ? "${var.prefix}-prometheus-full" : "notenest-prometheus-container"
  image = "prom/prometheus:v2.47.0"

  command = [
    "--config.file=/etc/prometheus/prometheus.yml",
    "--storage.tsdb.path=/prometheus",
    "--web.enable-lifecycle"
  ]

  volumes {
    host_path      = abspath("${path.module}/../../../prometheus/prometheus.yml")
    container_path = "/etc/prometheus/prometheus.yml"
  }

  volumes {
    host_path      = abspath("${path.module}/../../../prometheus/alerts.yml")
    container_path = "/etc/prometheus/alerts.yml"
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["prometheus-full"]
  }
}

resource "docker_container" "alertmanager" {
  name  = var.is_test ? "${var.prefix}-alertmanager" : "notenest-alertmanager-container"
  image = "prom/alertmanager:v0.27.0"

  command = [
    "--config.file=/etc/alertmanager/alertmanager.yml"
  ]

  volumes {
    host_path      = abspath("${path.module}/../../../alertmanager/alertmanager.yml")
    container_path = "/etc/alertmanager/alertmanager.yml"
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["alertmanager"]
  }
}

resource "docker_image" "loki" {
  name         = "grafana/loki:2.9.4"
  keep_locally = true
}

resource "docker_container" "loki" {
  name  = var.is_test ? "${var.prefix}-loki" : "notenest-loki-container"
  image = docker_image.loki.image_id

  command = [
    "-config.file=/etc/loki/loki-config.yml"
  ]

  volumes {
    host_path      = abspath("${path.module}/../../../loki/loki-config.yml")
    container_path = "/etc/loki/loki-config.yml"
  }

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 3100
      external = 3100
    }
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["loki"]
  }
}

resource "docker_image" "tempo" {
  name         = "grafana/tempo:2.4.1"
  keep_locally = true
}

resource "docker_container" "tempo" {
  name  = var.is_test ? "${var.prefix}-tempo" : "notenest-tempo-container"
  image = docker_image.tempo.image_id

  command = [
    "-config.file=/etc/tempo/tempo.yml"
  ]

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 3200
      external = 3200
    }
  }

  volumes {
    host_path      = abspath("${path.module}/../../../tempo/tempo.yml")
    container_path = "/etc/tempo/tempo.yml"
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["tempo"]
  }
}

resource "docker_container" "otel_collector" {
  name  = var.is_test ? "${var.prefix}-otel-collector" : "notenest-otel-collector-container"
  image = "otel/opentelemetry-collector-contrib:0.90.0"

  command = [
    "--config=/etc/otel-collector-config.yaml"
  ]

  volumes {
    host_path      = abspath("${path.module}/../../../otel-collector/otel-collector-config.yml")
    container_path = "/etc/otel-collector-config.yaml"
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["otel-collector"]
  }
}

resource "docker_image" "grafana" {
  name         = "grafana/grafana:10.2.0"
  keep_locally = true
}

resource "docker_container" "grafana" {
  name  = var.is_test ? "${var.prefix}-grafana" : "notenest-grafana-container"
  image = docker_image.grafana.image_id

  dynamic "ports" {
    for_each = var.is_test ? [] : [1]
    content {
      internal = 3000
      external = 3000
    }
  }

  networks_advanced {
    name    = var.mgmt_network_name
    aliases = ["grafana"]
  }
}

resource "docker_container" "bastion" {
  name    = var.is_test ? "${var.prefix}-bastion" : "notenest-bastion-container"
  image   = "alpine:latest"
  command = ["sleep", "infinity"]

  networks_advanced {
    name = var.public_network_name
  }
  networks_advanced {
    name = var.app_network_name
  }
  networks_advanced {
    name = var.data_network_name
  }
  networks_advanced {
    name = var.mgmt_network_name
  }
}
