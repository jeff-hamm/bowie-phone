FROM node:20-alpine

WORKDIR /app

# Install curl for healthcheck
RUN apk add --no-cache curl

# Copy package files
COPY package*.json ./

# Install dependencies
RUN npm ci --only=production

# Copy server code
COPY server.js ./

# Create logs directory
RUN mkdir -p /app/logs

# Expose ports
EXPOSE 3000
EXPOSE 2323
EXPOSE 2324

# Run as non-root user (99:100 for Unraid compatibility)
RUN addgroup -g 100 -S appuser || true && \
    adduser -S appuser -u 99 -G appuser || adduser -S appuser -u 99 && \
    chown -R 99:100 /app

USER 99:100

CMD ["node", "server.js"]
