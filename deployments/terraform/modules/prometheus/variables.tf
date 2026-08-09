variable "container_name" {
  type        = string
  description = "Container name"
}

variable "network_name" {
  type        = string
  description = "Docker network to attach container"
}

variable "host_port" {
  type        = number
  description = "Host port for Prometheus"
  default     = 9090
}
