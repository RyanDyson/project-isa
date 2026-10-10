"use client";

import { useState } from "react";
import { useForm } from "@tanstack/react-form";
import { useRouter } from "next/navigation";
import { z } from "zod";

import { authClient } from "@/server/better-auth/client";
import { cn } from "@/lib/utils";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Card, CardTitle } from "@/components/ui/card";
import {
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
} from "@/components/ui/card";

const credentials = z.object({
  email: z.string().email("Enter a valid email"),
  password: z.string().min(8, "At least 8 characters"),
});

export function AuthForm() {
  const [mode, setMode] = useState<"sign-in" | "sign-up">("sign-in");
  const [error, setError] = useState<string | null>(null);
  const router = useRouter();

  const form = useForm({
    defaultValues: {
      name: "",
      email: "",
      password: "",
    },
    onSubmit: async ({ value }) => {
      setError(null);
      const result =
        mode === "sign-up"
          ? await authClient.signUp.email({
              name: value.name,
              email: value.email,
              password: value.password,
            })
          : await authClient.signIn.email({
              email: value.email,
              password: value.password,
            });

      if (result.error) {
        setError(result.error.message ?? "Something went wrong");
        return;
      }

      router.push("/dashboard");
    },
  });

  return (
    <Card className="min-w-80 z-50 bg-background/90 backdrop-blur-md ">
      <form
        className="flex min-w-72 flex-col gap-4"
        onSubmit={(e) => {
          e.preventDefault();
          e.stopPropagation();
          void form.handleSubmit();
        }}
      >
        <CardHeader>
          <CardTitle>
            {mode === "sign-in" ? "Sign in" : "Create an account"}
          </CardTitle>
          <CardDescription>
            {mode === "sign-in"
              ? "Enter your credentials to access the exchange."
              : "Register to start using the exchange."}
          </CardDescription>
        </CardHeader>

        <CardContent className="flex flex-col gap-4">
          {mode === "sign-up" && (
            <form.Field name="name">
              {(field) => (
                <div className="flex flex-col gap-1.5">
                  <label htmlFor="name" className="text-xs font-medium">
                    Name
                  </label>
                  <Input
                    id="name"
                    type="text"
                    value={field.state.value}
                    onBlur={field.handleBlur}
                    onChange={(e) => field.handleChange(e.target.value)}
                    aria-invalid={!!field.state.meta.errors.length}
                  />
                  {field.state.meta.errors.length > 0 && (
                    <p className="text-destructive text-xs">
                      {field.state.meta.errors[0]}
                    </p>
                  )}
                </div>
              )}
            </form.Field>
          )}

          <form.Field
            name="email"
            validators={{
              onChange: ({ value }) =>
                credentials.shape.email.safeParse(value).error?.issues[0]
                  ?.message,
            }}
          >
            {(field) => (
              <div className="flex flex-col gap-1.5">
                <label htmlFor="email" className="text-xs font-medium">
                  Email
                </label>
                <Input
                  id="email"
                  type="email"
                  autoComplete="email"
                  value={field.state.value}
                  onBlur={field.handleBlur}
                  onChange={(e) => field.handleChange(e.target.value)}
                  aria-invalid={!!field.state.meta.errors.length}
                />
                {field.state.meta.errors.length > 0 && (
                  <p className="text-destructive text-xs">
                    {field.state.meta.errors[0]}
                  </p>
                )}
              </div>
            )}
          </form.Field>

          <form.Field
            name="password"
            validators={{
              onChange: ({ value }) =>
                credentials.shape.password.safeParse(value).error?.issues[0]
                  ?.message,
            }}
          >
            {(field) => (
              <div className="flex flex-col gap-1.5">
                <label htmlFor="password" className="text-xs font-medium">
                  Password
                </label>
                <Input
                  id="password"
                  type="password"
                  autoComplete={
                    mode === "sign-in" ? "current-password" : "new-password"
                  }
                  value={field.state.value}
                  onBlur={field.handleBlur}
                  onChange={(e) => field.handleChange(e.target.value)}
                  aria-invalid={!!field.state.meta.errors.length}
                />
                {field.state.meta.errors.length > 0 && (
                  <p className="text-destructive text-xs">
                    {field.state.meta.errors[0]}
                  </p>
                )}
              </div>
            )}
          </form.Field>

          {error && <p className="text-destructive text-xs">{error}</p>}
        </CardContent>

        <CardFooter className="flex-col gap-2">
          <Button
            type="submit"
            variant="gradient_primary"
            className="w-full"
            disabled={form.state.isSubmitting}
          >
            {form.state.isSubmitting
              ? "Submitting..."
              : mode === "sign-in"
                ? "Sign in"
                : "Sign up"}
          </Button>

          <Button
            type="button"
            variant="ghost"
            className={cn("text-muted-foreground")}
            onClick={() => {
              setError(null);
              form.reset();
              setMode(mode === "sign-in" ? "sign-up" : "sign-in");
            }}
          >
            {mode === "sign-in"
              ? "No account? Sign up instead"
              : "Already registered? Sign in"}
          </Button>
        </CardFooter>
      </form>
    </Card>
  );
}
