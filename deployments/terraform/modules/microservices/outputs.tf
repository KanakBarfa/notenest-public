output "auth_container_name" {
  description = "Name of Auth microservice container"
  value       = docker_container.auth.name
}

output "user_container_name" {
  description = "Name of User microservice container"
  value       = docker_container.user.name
}

output "graphql_container_name" {
  description = "Name of GraphQL microservice container"
  value       = docker_container.graphql.name
}
