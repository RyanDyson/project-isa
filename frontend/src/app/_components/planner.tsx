import { Panel } from "@/app/_components/panel";
import { cn } from "@/lib/utils";

type Policy = {
  name: string;
  value: number;
  unit: string;
  min: number;
  max: number;
  delta: number;
};

const POLICIES: Policy[] = [
  { name: "Fee per lot", value: 0.02, unit: "$", min: 0, max: 0.1, delta: 0.005 },
  { name: "Quote jitter penalty", value: 0.35, unit: "", min: 0, max: 1, delta: 0.02 },
  { name: "Max order size", value: 500, unit: "sh", min: 100, max: 1000, delta: 0 },
  { name: "Cancellation throttle", value: 0.6, unit: "", min: 0, max: 1, delta: -0.05 },
];

export function PlannerPanel() {
  return (
    <Panel
      title="RL Planner · Current Policy"
      action={
        <span className="rounded-full border border-amber-500/30 bg-amber-500/10 px-2 py-0.5 text-xs text-amber-500">
          adjusting
        </span>
      }
    >
      <div className="space-y-4">
        {POLICIES.map((p) => {
          const pctRaw = ((p.value - p.min) / (p.max - p.min)) * 100;
          const pctPct = Math.max(0, Math.min(100, pctRaw));
          return (
            <div key={p.name}>
              <div className="mb-1 flex items-baseline justify-between text-xs">
                <span className="text-muted-foreground">{p.name}</span>
                <span className="font-medium tabular-nums">
                  {p.unit === "$" ? "$" : ""}
                  {p.value}
                  {p.unit !== "$" ? ` ${p.unit}` : ""}
                  {p.delta !== 0 && (
                    <span
                      className={cn(
                        "ml-2 tabular-nums",
                        p.delta > 0 ? "text-emerald-500" : "text-red-500",
                      )}
                    >
                      {p.delta > 0 ? "+" : ""}
                      {p.delta}
                    </span>
                  )}
                </span>
              </div>
              <div className="h-1.5 rounded-full bg-muted">
                <div
                  className="h-1.5 rounded-full bg-amber-500 transition-all"
                  style={{ width: `${pctPct}%` }}
                />
              </div>
            </div>
          );
        })}
      </div>
    </Panel>
  );
}
