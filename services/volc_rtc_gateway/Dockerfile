FROM node:22.19.0-alpine3.22

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev

COPY src ./src
COPY config ./config

ENV HOST=0.0.0.0
ENV PORT=8080
EXPOSE 8080
USER node

CMD ["node", "src/server.js"]
