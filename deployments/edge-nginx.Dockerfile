FROM nginx:alpine

RUN mkdir -p /var/cache/nginx/cdn && chown -R nginx:nginx /var/cache/nginx/cdn

COPY deployments/edge-nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 8081

CMD ["nginx", "-g", "daemon off;"]
