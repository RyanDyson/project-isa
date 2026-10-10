import "server-only";

import { headers } from "next/headers";
import { cache } from "react";

import { env } from "@/env";

/**
 * Session shape returned by the Python backend's Better Auth-compatible
 * /api/auth/get-session endpoint.
 */
export type Session = {
  user: {
    id: string;
    name: string;
    email: string;
    emailVerified: boolean;
    image: string | null;
  };
  session: {
    id: string;
    userId: string;
    expiresAt: string;
  };
} | null;

/**
 * Server-side session lookup: forwards the browser cookies to the FastAPI
 * backend so RSC pages can gate rendering on an active session.
 */
export const getSession = cache(async (): Promise<Session> => {
  const heads = new Headers(await headers());
  const cookie = heads.get("cookie") ?? "";

  const response = await fetch(`${env.BACKEND_URL}/api/auth/get-session`, {
    headers: { cookie },
    cache: "no-store",
  });

  if (!response.ok) return null;

  const body: unknown = await response.json();
  return body as Session;
});
