variable "container_name" {
  type        = string
  description = "Container name for Redis"
}

variable "network_name" {
  type        = string
  description = "Docker network name"
}

variable "host_port" {
  type        = number
  description = "Host port for Redis"
  default     = 6379
}

variable "data_network_name" {
  type        = string
  description = "Data network name"
  default     = "data_net"
}
