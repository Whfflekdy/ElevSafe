FROM node:20-alpine
WORKDIR /app
COPY package.json ./
RUN npm install --omit=dev
COPY relay.js ./
USER node
CMD ["node", "relay.js"]