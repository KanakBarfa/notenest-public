variable "container_name" {
  type        = string
  description = "Container name"
}

variable "network_name" {
  type        = string
  description = "Docker network to attach container"
}

variable "postgres_user" {
  type        = string
  description = "PostgreSQL user"
  default     = "notenest"
}

variable "postgres_password" {
  type        = string
  description = "PostgreSQL password"
  sensitive   = true
  default     = "notenest_pass"
}

variable "postgres_db" {
  type        = string
  description = "PostgreSQL database"
  default     = "notenest"
}

variable "host_port" {
  type        = number
  description = "Host port for PostgreSQL"
  default     = 5432
}
