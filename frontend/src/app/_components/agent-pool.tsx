import { Panel } from "@/app/_components/panel";

import { cn } from "@/lib/utils";

type Agent = {
  id: string;
  type: "market-making" | "momentum" | "spoofing" | "rl-trading" | "rl-planner";
  pnl: number;
  orders: number;
  active: boolean;
};

const AGENTS: Agent[] = [
  { id: "MM-01", type: "market-making", pnl: 412.5, orders: 1842, active: true },
  { id: "MM-02", type: "market-making", pnl: 388.1, orders: 1709, active: true },
  { id: "MO-01", type: "momentum", pnl: -120.4, orders: 512, active: true },
  { id: "SP-01", type: "spoofing", pnl: -28.7, orders: 2931, active: true },
  { id: "RL-01", type: "rl-trading", pnl: 156.2, orders: 744, active: true },
  { id: "PL-01", type: "rl-planner", pnl: 0, orders: 0, active: false },
];

const typeLabel: Record<Agent["type"], string> = {
  "market-making": "Market Making",
  momentum: "Momentum",
  spoofing: "Spoofing",
  "rl-trading": "RL Trading",
  "rl-planner": "RL Planner",
};

const typeColor: Record<Agent["type"], string> = {
  "market-making": "bg-sky-500/10 text-sky-500 border-sky-500/30",
  momentum: "bg-violet-500/10 text-violet-500 border-violet-500/30",
  spoofing: "bg-red-500/10 text-red-500 border-red-500/30",
  "rl-trading": "bg-emerald-500/10 text-emerald-500 border-emerald-500/30",
  "rl-planner": "bg-amber-500/10 text-amber-500 border-amber-500/30",
};

export function AgentPool({ className }: { className?: string }) {
  return (
    <Panel
      className={className}
      title="Agent Pool"
      action={<span className="text-xs text-muted-foreground">{AGENTS.filter((a) => a.active).length}/{AGENTS.length} active</span>}
    >
      <div className="grid grid-cols-[1fr_auto_auto] gap-x-4 gap-y-2 text-xs">
        <span className="text-muted-foreground">Agent</span>
        <span className="text-right text-muted-foreground">P&L ($)</span>
        <span className="text-right text-muted-foreground">Orders</span>
        {AGENTS.map((a) => (
          <div key={a.id} className="col-span-3 grid grid-cols-[1fr_auto_auto] items-center gap-x-4">
            <div className="flex min-w-0 items-center gap-2">
              <span className="truncate font-medium">{a.id}</span>
              <span
                className={cn(
                  "hidden shrink-0 rounded-full border px-1.5 py-0.5 sm:inline",
                  typeColor[a.type],
                )}
              >
                {typeLabel[a.type]}
              </span>
              {!a.active && (
                <span className="shrink-0 rounded-full border border-border px-1.5 py-0.5 text-muted-foreground">
                  idle
                </span>
              )}
            </div>
            <span
              className={cn(
                "text-right tabular-nums",
                a.pnl > 0
                  ? "text-emerald-500"
                  : a.pnl < 0
                    ? "text-red-500"
                    : "text-muted-foreground",
              )}
            >
              {a.pnl.toFixed(2)}
            </span>
            <span className="text-right tabular-nums text-muted-foreground">
              {a.orders.toLocaleString()}
            </span>
          </div>
        ))}
      </div>
    </Panel>
  );
}