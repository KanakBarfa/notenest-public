output "container_id" {
  description = "ID of the Prometheus container"
  value       = docker_container.prometheus.id
}

output "container_name" {
  description = "Name of the Prometheus container"
  value       = docker_container.prometheus.name
}
