output "vpc_id" {
  description = "The ID of the AWS VPC"
  value       = aws_vpc.main.id
}

output "rds_endpoint" {
  description = "The connection endpoint for Amazon RDS PostgreSQL"
  value       = aws_db_instance.postgres.endpoint
}

output "redis_endpoint" {
  description = "The connection endpoint for Amazon ElastiCache Redis"
  value       = aws_elasticache_cluster.redis.cache_nodes[0].address
}

output "s3_bucket_name" {
  description = "The name of the AWS S3 attachments bucket"
  value       = aws_s3_bucket.attachments.id
}

output "ecr_repository_urls" {
  description = "Map of ECR repository URLs for Docker image pushes"
  value       = { for k, v in aws_ecr_repository.microservices : k => v.repository_url }
}
