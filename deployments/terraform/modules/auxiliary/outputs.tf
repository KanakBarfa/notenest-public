output "pgbouncer_container_name" {
  description = "Name of PgBouncer container"
  value       = docker_container.pgbouncer.name
}

output "edge_nginx_container_name" {
  description = "Name of Edge Nginx CDN container"
  value       = docker_container.edge_nginx.name
}

output "kong_container_name" {
  description = "Name of Kong API Gateway container"
  value       = docker_container.kong.name
}

output "haproxy_container_name" {
  description = "Name of HAProxy container"
  value       = docker_container.haproxy.name
}
