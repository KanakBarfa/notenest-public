terraform {
  required_version = ">= 1.0.0"

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

provider "docker" {}

locals {
  env     = terraform.workspace != "default" ? terraform.workspace : (var.environment != "" ? var.environment : "dev")
  prefix  = "notenest-${local.env}"
  is_test = can(regex("^e2e-", local.env))

  postgres_port   = local.is_test ? 0 : var.postgres_port
  kafka_port      = local.is_test ? 0 : var.kafka_port
  prometheus_port = local.is_test ? 0 : var.prometheus_port
  web_port        = local.is_test ? 0 : 8082

  public_net_name = local.is_test ? "${local.prefix}-public-net" : "public_net"
  app_net_name    = local.is_test ? "${local.prefix}-app-net" : "app_net"
  data_net_name   = local.is_test ? "${local.prefix}-data-net" : "data_net"
  mgmt_net_name   = local.is_test ? "${local.prefix}-mgmt-net" : "mgmt_net"
}

resource "docker_network" "tf_network" {
  name = "${local.prefix}-network"
}

resource "docker_network" "public_net" {
  name = local.public_net_name
  ipam_config {
    subnet  = local.is_test ? "172.28.0.0/24" : "172.20.0.0/24"
    gateway = local.is_test ? "172.28.0.1" : "172.20.0.1"
  }
  lifecycle {
    ignore_changes = [ipam_config]
  }
}

resource "docker_network" "app_net" {
  name = local.app_net_name
  ipam_config {
    subnet  = local.is_test ? "172.28.1.0/24" : "172.20.1.0/24"
    gateway = local.is_test ? "172.28.1.1" : "172.20.1.1"
  }
  lifecycle {
    ignore_changes = [ipam_config]
  }
}

resource "docker_network" "data_net" {
  name = local.data_net_name
  ipam_config {
    subnet  = local.is_test ? "172.28.2.0/24" : "172.20.2.0/24"
    gateway = local.is_test ? "172.28.2.1" : "172.20.2.1"
  }
  lifecycle {
    ignore_changes = [ipam_config]
  }
}

resource "docker_network" "mgmt_net" {
  name = local.mgmt_net_name
  ipam_config {
    subnet  = local.is_test ? "172.28.3.0/24" : "172.20.3.0/24"
    gateway = local.is_test ? "172.28.3.1" : "172.20.3.1"
  }
  lifecycle {
    ignore_changes = [ipam_config]
  }
}

module "postgres" {
  source            = "./modules/postgres"
  container_name    = local.is_test ? "${local.prefix}-postgres" : "notenest-db-container"
  network_name      = docker_network.tf_network.name
  postgres_user     = var.postgres_user
  postgres_password = var.postgres_password
  postgres_db       = var.postgres_db
  host_port         = local.postgres_port
}

module "kafka" {
  source         = "./modules/kafka"
  container_name = local.is_test ? "${local.prefix}-kafka" : "notenest-kafka-container"
  network_name   = docker_network.tf_network.name
  host_port      = local.kafka_port
}

module "prometheus" {
  source         = "./modules/prometheus"
  container_name = "${local.prefix}-prometheus"
  network_name   = docker_network.tf_network.name
  host_port      = local.prometheus_port
}

module "redis" {
  source            = "./modules/redis"
  container_name    = "${local.prefix}-redis"
  network_name      = docker_network.tf_network.name
  data_network_name = docker_network.data_net.name
  host_port         = local.is_test ? 0 : 6379
}

module "minio" {
  source            = "./modules/minio"
  container_name    = "${local.prefix}-minio"
  network_name      = docker_network.tf_network.name
  data_network_name = docker_network.data_net.name
  host_port         = local.is_test ? 0 : 9000
  console_port      = local.is_test ? 0 : 9001
}

module "consul" {
  source            = "./modules/consul"
  container_name    = "${local.prefix}-consul"
  network_name      = docker_network.tf_network.name
  mgmt_network_name = docker_network.mgmt_net.name
  host_port         = local.is_test ? 0 : 8500
}

module "microservices" {
  source                  = "./modules/microservices"
  prefix                  = local.prefix
  network_name            = docker_network.tf_network.name
  app_network_name        = docker_network.app_net.name
  postgres_container_name = module.postgres.container_name
  redis_container_name    = module.redis.container_name
  kafka_container_name    = module.kafka.container_name
  consul_container_name   = local.is_test ? "${local.prefix}-consul" : "notenest-dev-consul"
  app_container_name      = local.is_test ? "${local.prefix}-app" : "notenest-app-container"
  is_test                 = local.is_test
}

module "auxiliary" {
  source                  = "./modules/auxiliary"
  prefix                  = local.prefix
  public_network_name     = docker_network.public_net.name
  app_network_name        = docker_network.app_net.name
  data_network_name       = docker_network.data_net.name
  mgmt_network_name       = docker_network.mgmt_net.name
  postgres_container_name = module.postgres.container_name
  is_test                 = local.is_test
}

module "app" {
  source                  = "./modules/app"
  container_name          = local.is_test ? "${local.prefix}-app" : "notenest-app-container"
  nginx_container_name    = local.is_test ? "${local.prefix}-nginx" : "notenest-nginx-container"
  network_name            = docker_network.tf_network.name
  public_network_name     = docker_network.public_net.name
  app_network_name        = docker_network.app_net.name
  data_network_name       = docker_network.data_net.name
  postgres_container_name = module.postgres.container_name
  redis_container_name    = module.redis.container_name
  minio_container_name    = module.minio.container_name
  kafka_container_name    = module.kafka.container_name
  web_port                = local.web_port

  depends_on = [
    module.auxiliary
  ]
}
