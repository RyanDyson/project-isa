import { redirect } from "next/navigation";

import { AuthForm } from "@/components/auth/auth-form";
import { getSession } from "@/server/better-auth/server";

export default async function Home() {
  const session = await getSession();

  if (session) redirect("/dashboard");

  return (
    <main className="bg-primary/10 flex h-full min-h-screen w-full items-center justify-center p-4">
      <AuthForm />
    </main>
  );
}
