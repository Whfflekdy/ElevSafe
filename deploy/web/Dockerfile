FROM node:20-alpine AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY . .
RUN CI=false npm run build

FROM nginx:1.27-alpine
COPY --from=build /app/build /usr/share/nginx/html