import { redirect } from "next/navigation";

import { getSession } from "@/server/auth/session";

export default async function Dashboard() {
  const session = await getSession();

  if (!session) redirect("/");

  return (
    <main className="bg-background flex min-h-screen flex-col">
      
    </main>
  );
}