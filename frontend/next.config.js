/**
 * Run `build` or `dev` with `SKIP_ENV_VALIDATION` to skip env validation. This is especially useful
 * for Docker builds.
 */
import "./src/env.js";

import path from "node:path";
import { fileURLToPath } from "node:url";

const dirname = path.dirname(fileURLToPath(import.meta.url));

const backendUrl = process.env.BACKEND_URL ?? "http://localhost:8000";

/** @type {import("next").NextConfig} */
const config = {
  // Monorepo: pin Turbopack root so the repo-level bun.lock isn't picked up
  turbopack: { root: dirname },
  // Better Auth handlers and everything else are served by the FastAPI backend
  async rewrites() {
    return [
      {
        source: "/api/auth/:path*",
        destination: `${backendUrl}/api/auth/:path*`,
      },
    ];
  },
};

export default config;
