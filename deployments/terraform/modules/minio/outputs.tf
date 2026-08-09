output "container_id" {
  description = "ID of the MinIO container"
  value       = docker_container.minio.id
}

output "container_name" {
  description = "Name of the MinIO container"
  value       = docker_container.minio.name
}
