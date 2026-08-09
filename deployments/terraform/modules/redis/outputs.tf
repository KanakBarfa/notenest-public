output "container_id" {
  description = "ID of the Redis container"
  value       = docker_container.redis.id
}

output "container_name" {
  description = "Name of the Redis container"
  value       = docker_container.redis.name
}
