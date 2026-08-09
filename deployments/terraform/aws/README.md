# NoteNest AWS Production Infrastructure Blueprint (Terraform)

This directory contains a complete, production-ready Infrastructure as Code (IaC) blueprint for provisioning NoteNest onto Amazon Web Services (AWS) using Terraform.

---

## 1. Architecture Components
* **Networking (AWS VPC):** 4-tier segmented subnets across 2 Availability Zones (`public`, `app`, `data`).
* **Relational Database (Amazon RDS):** Managed PostgreSQL 15 database (`db.t3.micro`).
* **Caching (Amazon ElastiCache):** Managed Redis 7 cluster (`cache.t3.micro`).
* **Object Storage (Amazon S3):** CORS-configured S3 bucket for note attachments.
* **Container Registry (Amazon ECR):** Dedicated ECR repositories for all 6 NoteNest microservice container images (`app`, `auth`, `user`, `notification`, `graphql`, `nginx`).

---

## 2. Deployment Instructions

### Step 1: Configure AWS CLI & Authenticate
```bash
aws configure
# Enter your AWS Access Key ID, Secret Access Key, and default region (e.g. us-east-1)
```

### Step 2: Initialize & Apply Terraform
```bash
cd deployments/terraform/aws
terraform init
terraform plan -out=tfplan
terraform apply -auto-approve tfplan
```

### Step 3: Push Docker Images to ECR
```bash
# Get ECR login token
aws ecr get-login-password --region us-east-1 | docker login --username AWS --password-stdin <AWS_ACCOUNT_ID>.dkr.ecr.us-east-1.amazonaws.com

# Tag and push app image
docker tag notenest-app:latest <AWS_ACCOUNT_ID>.dkr.ecr.us-east-1.amazonaws.com/notenest-prod-notenest-app:latest
docker push <AWS_ACCOUNT_ID>.dkr.ecr.us-east-1.amazonaws.com/notenest-prod-notenest-app:latest
```

### Step 4: Deploy NoteNest Helm Chart to Kubernetes
```bash
helm upgrade --install notenest ../../helm/notenest \
  --set db.host=$(terraform output -raw rds_endpoint | cut -d: -f1) \
  --set redis.host=$(terraform output -raw redis_endpoint) \
  --set minio.endpoint=$(terraform output -raw s3_bucket_name).s3.amazonaws.com
```

### Step 5: Teardown AWS Resources
To destroy all provisioned AWS cloud resources and avoid incurring charges:
```bash
terraform destroy -auto-approve
```
