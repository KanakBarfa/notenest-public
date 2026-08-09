FROM node:22-alpine

WORKDIR /app

COPY package*.json ./

RUN npm install --only=production

COPY src ./src

EXPOSE 4000

CMD ["node", "src/index.js"]
