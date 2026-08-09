variable "prefix" {
  type        = string
  description = "Resource prefix"
}

variable "public_network_name" {
  type        = string
  description = "Public network name"
}

variable "app_network_name" {
  type        = string
  description = "App network name"
}

variable "data_network_name" {
  type        = string
  description = "Data network name"
}

variable "mgmt_network_name" {
  type        = string
  description = "Management network name"
}

variable "postgres_container_name" {
  type        = string
  description = "Postgres container name"
}

variable "is_test" {
  type        = bool
  description = "Is test workspace"
  default     = false
}
