output "container_id" {
  description = "ID of the Consul container"
  value       = docker_container.consul.id
}

output "container_name" {
  description = "Name of the Consul container"
  value       = docker_container.consul.name
}
