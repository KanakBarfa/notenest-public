variable "container_name" {
  type        = string
  description = "Container name for Consul"
}

variable "network_name" {
  type        = string
  description = "Docker network name"
}

variable "host_port" {
  type        = number
  description = "Host port for Consul HTTP API"
  default     = 8500
}

variable "mgmt_network_name" {
  type        = string
  description = "Management network name"
  default     = "mgmt_net"
}
