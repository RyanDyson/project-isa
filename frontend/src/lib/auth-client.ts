"use client";

/**
 * Minimal email/password auth client for the FastAPI backend's
 * Better Auth-compatible `/api/auth/*` endpoints (proxied from the Next.js dev
 * server). The response/error contract mirrors better-auth's client so callers
 * get `{ data, error }`.
 */

export type User = {
  id: string;
  name: string;
  email: string;
  emailVerified: boolean;
  image: string | null;
};

export type AuthError = {
  code: string;
  message: string;
  status: number;
};

type AuthResult<T> = { data: T | null; error: AuthError | null };

async function post(path: string, body: Record<string, string>): Promise<AuthResult<unknown>> {
  const response = await fetch(`/api/auth/${path}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });

  if (!response.ok) {
    const body = (await response.json().catch(() => null)) as { message?: string; code?: string } | null;
    return {
      data: null,
      error: {
        code: body?.code ?? "UNKNOWN",
        message: body?.message ?? "Something went wrong",
        status: response.status,
      },
    };
  }

  return { data: await response.json(), error: null };
}

export const authClient = {
  signUp: {
    email: async (input: { name: string; email: string; password: string }) =>
      post("sign-up/email", input),
  },
  signIn: {
    email: async (input: { email: string; password: string }) =>
      post("sign-in/email", input),
  },
};
