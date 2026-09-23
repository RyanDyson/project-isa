import { AgentPool } from "@/app/_components/agent-pool";
import { Header } from "@/app/_components/header";
import { MetricsBar } from "@/app/_components/metrics-bar";
import { OrderBook } from "@/app/_components/order-book";
import { PlannerPanel } from "@/app/_components/planner";
import { PriceChart } from "@/app/_components/price-chart";
import { TradeTape } from "@/app/_components/trade-tape";

export default function Dashboard() {
  return (
    <main className="flex min-h-screen flex-col bg-background">
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
