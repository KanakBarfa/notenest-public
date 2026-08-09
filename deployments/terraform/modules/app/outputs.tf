output "app_container_id" {
  description = "ID of the backend app container"
  value       = docker_container.app.id
}

output "nginx_container_id" {
  description = "ID of the nginx web frontend container"
  value       = docker_container.nginx.id
}

output "nginx_container_name" {
  description = "Name of the nginx web frontend container"
  value       = docker_container.nginx.name
}
