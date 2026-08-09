output "postgres_container_name" {
  description = "Name of the provisioned PostgreSQL container"
  value       = module.postgres.container_name
}

output "kafka_container_name" {
  description = "Name of the provisioned Kafka container"
  value       = module.kafka.container_name
}

output "prometheus_container_name" {
  description = "Name of the provisioned Prometheus container"
  value       = module.prometheus.container_name
}

output "web_frontend_container_name" {
  description = "Name of the provisioned Nginx web frontend container"
  value       = module.app.nginx_container_name
}

output "redis_container_name" {
  description = "Name of the provisioned Redis container"
  value       = module.redis.container_name
}

output "minio_container_name" {
  description = "Name of the provisioned MinIO container"
  value       = module.minio.container_name
}

output "workspace" {
  description = "Active Terraform workspace environment"
  value       = terraform.workspace
}
