FROM nginx:alpine

COPY deployments/origin-nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 9005

CMD ["nginx", "-g", "daemon off;"]
