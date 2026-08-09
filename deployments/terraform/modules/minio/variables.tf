variable "container_name" {
  type        = string
  description = "Container name for MinIO"
}

variable "network_name" {
  type        = string
  description = "Docker network name"
}

variable "host_port" {
  type        = number
  description = "Host port for MinIO API"
  default     = 9000
}

variable "console_port" {
  type        = number
  description = "Host port for MinIO Console"
  default     = 9001
}

variable "data_network_name" {
  type        = string
  description = "Docker data network name"
  default     = "data_net"
}
