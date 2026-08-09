output "container_id" {
  description = "ID of the Kafka container"
  value       = docker_container.kafka.id
}

output "container_name" {
  description = "Name of the Kafka container"
  value       = docker_container.kafka.name
}
