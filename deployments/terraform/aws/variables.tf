variable "aws_region" {
  type        = string
  description = "AWS region for deployment"
  default     = "us-east-1"
}

variable "environment" {
  type        = string
  description = "Deployment environment name (e.g. dev, stage, prod)"
  default     = "prod"
}

variable "vpc_cidr" {
  type        = string
  description = "CIDR block for the AWS VPC"
  default     = "10.0.0.0/16"
}

variable "eks_cluster_name" {
  type        = string
  description = "Name of the AWS EKS cluster"
  default     = "notenest-eks-cluster"
}

variable "db_name" {
  type        = string
  description = "PostgreSQL database name"
  default     = "postgres"
}

variable "db_username" {
  type        = string
  description = "PostgreSQL master username"
  default     = "postgres"
}

variable "db_password" {
  type        = string
  description = "PostgreSQL master password (must be passed via TF_VAR_db_password or terraform.tfvars)"
  sensitive   = true
}

variable "s3_bucket_name" {
  type        = string
  description = "AWS S3 bucket name for NoteNest attachments"
  default     = "notenest-attachments-prod"
}
