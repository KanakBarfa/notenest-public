variable "container_name" {
  type        = string
  description = "Container name for backend app"
}

variable "nginx_container_name" {
  type        = string
  description = "Container name for frontend nginx"
}

variable "network_name" {
  type        = string
  description = "Docker network name"
}

variable "web_port" {
  type        = number
  description = "Host port for web UI"
  default     = 80
}

variable "postgres_container_name" {
  type        = string
  description = "Postgres container hostname"
}

variable "redis_container_name" {
  type        = string
  description = "Redis container hostname"
  default     = "notenest-dev-redis"
}

variable "minio_container_name" {
  type        = string
  description = "MinIO container hostname"
  default     = "notenest-dev-minio"
}

variable "kafka_container_name" {
  type        = string
  description = "Kafka container hostname"
  default     = "notenest-dev-kafka"
}

variable "public_network_name" {
  type        = string
  description = "Public network name"
  default     = "public_net"
}

variable "app_network_name" {
  type        = string
  description = "App network name"
  default     = "app_net"
}

variable "data_network_name" {
  type        = string
  description = "Data network name"
  default     = "data_net"
}

variable "jwt_secret" {
  description = "Generated JWT signing secret shared by all services"
  type        = string
}
