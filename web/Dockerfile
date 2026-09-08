FROM node:20.19-alpine

WORKDIR /app

# Only the manifests in this layer: the source is bind-mounted by compose,
# so copying it here just busts the npm cache on every edit (WP 00 §8).
COPY web/package.json web/package-lock.json web/.npmrc ./
RUN npm ci

EXPOSE 5173
CMD ["npm", "run", "dev", "--", "--host", "0.0.0.0", "--port", "5173"]
