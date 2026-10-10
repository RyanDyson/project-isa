import { redirect } from "next/navigation";

import { Header } from "@/components/dashboard/header";
import { getSession } from "@/server/better-auth/server";

export default async function Dashboard() {
  const session = await getSession();

  if (!session) redirect("/");

  return (
    <main className="bg-background flex min-h-screen flex-col">
      <Header />
      <DashboardBody />
    </main>
  );
}

function DashboardBody() {
  return (
    <div className="flex flex-1 flex-col gap-4 p-6">
    </div>
  );
}
