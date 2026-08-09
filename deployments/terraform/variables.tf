variable "environment" {
  type        = string
  description = "Target environment (dev, stage, prod)"
  default     = "dev"
}

variable "postgres_user" {
  type        = string
  description = "PostgreSQL admin username (overridden by TF_VAR_postgres_user or .env)"
  default     = "postgres"
}

variable "postgres_password" {
  type        = string
  description = "PostgreSQL admin password (overridden by TF_VAR_postgres_password or .env)"
  sensitive   = true
  default     = "pass"
}

variable "postgres_db" {
  type        = string
  description = "PostgreSQL database name (overridden by TF_VAR_postgres_db or .env)"
  default     = "postgres"
}

variable "postgres_port" {
  type        = number
  description = "PostgreSQL host port mapping"
  default     = 5432
}

variable "kafka_port" {
  type        = number
  description = "Kafka broker host port mapping"
  default     = 9092
}

variable "prometheus_port" {
  type        = number
  description = "Prometheus web UI host port mapping"
  default     = 9090
}
