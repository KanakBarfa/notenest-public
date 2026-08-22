variable "prefix" {
  type        = string
  description = "Resource prefix"
}

variable "network_name" {
  type        = string
  description = "Docker network name"
}

variable "postgres_container_name" {
  type        = string
  description = "Postgres container name"
}

variable "redis_container_name" {
  type        = string
  description = "Redis container name"
}

variable "is_test" {
  type        = bool
  description = "Is test workspace"
  default     = false
}

variable "kafka_container_name" {
  type        = string
  description = "Kafka container name"
}

variable "consul_container_name" {
  type        = string
  description = "Consul container name"
}

variable "app_container_name" {
  type        = string
  description = "App container name"
}

variable "app_network_name" {
  type        = string
  description = "App network name for service-to-service communication"
}

variable "jwt_secret" {
  description = "Generated JWT signing secret shared by all services"
  type        = string
}
