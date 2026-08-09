terraform {
  required_version = ">= 1.5.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = var.aws_region

  default_tags {
    tags = {
      Project     = "NoteNest"
      Environment = var.environment
      ManagedBy   = "Terraform"
    }
  }
}

data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  prefix = "notenest-${var.environment}"
  azs    = slice(data.aws_availability_zones.available.names, 0, 2)
}

# 1. VPC & Multi-Tier Network Segmentation (Public, App, Data)
resource "aws_vpc" "main" {
  cidr_block           = var.vpc_cidr
  enable_dns_hostnames = true
  enable_dns_support   = true

  tags = {
    Name = "${local.prefix}-vpc"
  }
}

resource "aws_internet_gateway" "gw" {
  vpc_id = aws_vpc.main.id

  tags = {
    Name = "${local.prefix}-igw"
  }
}

# Public Subnets (Ingress / Load Balancers)
resource "aws_subnet" "public" {
  count                   = 2
  vpc_id                  = aws_vpc.main.id
  cidr_block              = cidrsubnet(var.vpc_cidr, 4, count.index)
  availability_zone       = local.azs[count.index]
  map_public_ip_on_launch = true

  tags = {
    Name                                           = "${local.prefix}-public-${count.index + 1}"
    "kubernetes.io/role/elb"                       = "1"
    "kubernetes.io/cluster/${var.eks_cluster_name}" = "shared"
  }
}

# Private Application Subnets (EKS Worker Nodes & Microservices)
resource "aws_subnet" "private_app" {
  count             = 2
  vpc_id            = aws_vpc.main.id
  cidr_block        = cidrsubnet(var.vpc_cidr, 4, count.index + 2)
  availability_zone = local.azs[count.index]

  tags = {
    Name                                           = "${local.prefix}-app-${count.index + 1}"
    "kubernetes.io/role/internal-elb"              = "1"
    "kubernetes.io/cluster/${var.eks_cluster_name}" = "shared"
  }
}

# Database Subnets (RDS, ElastiCache)
resource "aws_subnet" "private_data" {
  count             = 2
  vpc_id            = aws_vpc.main.id
  cidr_block        = cidrsubnet(var.vpc_cidr, 4, count.index + 4)
  availability_zone = local.azs[count.index]

  tags = {
    Name = "${local.prefix}-data-${count.index + 1}"
  }
}

# Route Tables
resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.gw.id
  }

  tags = {
    Name = "${local.prefix}-public-rt"
  }
}

resource "aws_route_table_association" "public" {
  count          = 2
  subnet_id      = aws_subnet.public[count.index].id
  route_table_id = aws_route_table.public.id
}

# 2. Managed PostgreSQL Database (Amazon RDS)
resource "aws_db_subnet_group" "rds" {
  name       = "${local.prefix}-rds-subnet-group"
  subnet_ids = aws_subnet.private_data[*].id

  tags = {
    Name = "${local.prefix}-rds-subnet-group"
  }
}

resource "aws_security_group" "rds_sg" {
  name        = "${local.prefix}-rds-sg"
  description = "Allow PostgreSQL inbound traffic from EKS worker nodes"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port   = 5432
    to_port     = 5432
    protocol    = "tcp"
    cidr_blocks = [aws_vpc.main.cidr_block]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    ="-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}

resource "aws_db_instance" "postgres" {
  identifier             = "${local.prefix}-postgres"
  allocated_storage      = 20
  max_allocated_storage  = 100
  engine                 = "postgres"
  engine_version         = "15.4"
  instance_class         = "db.t3.micro"
  db_name                = var.db_name
  username               = var.db_username
  password               = var.db_password
  db_subnet_group_name   = aws_db_subnet_group.rds.name
  vpc_security_group_ids = [aws_security_group.rds_sg.id]
  skip_final_snapshot    = true
  publicly_accessible    = false
}

# 3. Managed Cache (Amazon ElastiCache Redis)
resource "aws_elasticache_subnet_group" "redis" {
  name       = "${local.prefix}-redis-subnet-group"
  subnet_ids = aws_subnet.private_data[*].id
}

resource "aws_security_group" "redis_sg" {
  name        = "${local.prefix}-redis-sg"
  description = "Allow Redis traffic from VPC"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port   = 6379
    to_port     = 6379
    protocol    = "tcp"
    cidr_blocks = [aws_vpc.main.cidr_block]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}

resource "aws_elasticache_cluster" "redis" {
  cluster_id           = "${local.prefix}-redis"
  engine               = "redis"
  node_type            = "cache.t3.micro"
  num_cache_nodes      = 1
  parameter_group_name = "default.redis7"
  port                 = 6379
  subnet_group_name    = aws_elasticache_subnet_group.redis.name
  security_group_ids   = [aws_security_group.redis_sg.id]
}

# 4. Attachment Object Storage (Amazon S3)
resource "aws_s3_bucket" "attachments" {
  bucket        = var.s3_bucket_name
  force_destroy = true
}

resource "aws_s3_bucket_cors_configuration" "attachments" {
  bucket = aws_s3_bucket.attachments.id

  cors_rule {
    allowed_headers = ["*"]
    allowed_methods = ["GET", "PUT", "POST", "DELETE"]
    allowed_origins = ["*"]
    max_age_seconds = 3600
  }
}

# 5. ECR Repositories for NoteNest Docker Images
resource "aws_ecr_repository" "microservices" {
  for_each = toset([
    "notenest-app",
    "notenest-auth",
    "notenest-user",
    "notenest-notification",
    "notenest-graphql",
    "notenest-nginx"
  ])

  name                 = "${local.prefix}-${each.key}"
  image_tag_mutability = "MUTABLE"

  image_scanning_configuration {
    scan_on_push = true
  }
}
