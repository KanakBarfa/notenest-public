# Stage 1: Build the frontend React app
FROM node:22-alpine AS builder

WORKDIR /app

# Copy dependency configs
COPY frontend/package.json frontend/package-lock.json* ./

# Install dependencies
RUN npm ci

# Copy source code
COPY frontend/ ./

# Build the application (production build)
RUN npm run build

# Stage 2: Serve using Nginx
FROM nginx:alpine

# Copy the build output from Stage 1 to Nginx's HTML serving folder
COPY --from=builder /app/dist /var/www/frontend

# Copy custom Nginx configuration
COPY deployments/default.conf /etc/nginx/conf.d/default.conf

EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
