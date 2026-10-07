import { redirect } from "next/navigation";

import { AgentPool } from "@/components/dashboard/agent-pool";
import { Header } from "@/components/dashboard/header";
import { MetricsBar } from "@/components/dashboard/metrics-bar";
import { OrderBook } from "@/components/dashboard/order-book";
import { PlannerPanel } from "@/components/dashboard/planner";
import { PriceChart } from "@/components/dashboard/price-chart";
import { TradeTape } from "@/components/dashboard/trade-tape";
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
      <MetricsBar />
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
        <PriceChart />
        <OrderBook />
      </div>
      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
        <AgentPool className="lg:col-span-2" />
        <TradeTape />
      </div>
      <PlannerPanel />
    </div>
  );
}
