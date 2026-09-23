import { cn } from "@/lib/utils";

const metrics = [
  { label: "Last Price", value: "101.42", delta: "+0.84%", positive: true },
  { label: "Volatility", value: "1.82%", delta: "+0.12", positive: false },
  { label: "Bid-Ask Spread", value: "0.042", delta: "-0.003", positive: true },
  { label: "Volume (1m)", value: "24,510", delta: "+1,204", positive: true },
  { label: "Cancellation Rate", value: "12.4%", delta: "+2.1%", positive: false },
  { label: "Order-to-Trade", value: "3.2x", delta: "-0.4x", positive: true },
];

export function MetricsBar() {
  return (
    <div className="grid grid-cols-2 gap-3 sm:grid-cols-3 xl:grid-cols-6">
      {metrics.map((m) => (
        <div
          key={m.label}
          className="rounded-lg border border-border bg-card px-4 py-3"
        >
          <p className="text-xs text-muted-foreground">{m.label}</p>
          <p className="mt-1 text-lg font-semibold tabular-nums">{m.value}</p>
          <p
            className={cn(
              "mt-0.5 text-xs tabular-nums",
              m.positive ? "text-emerald-500" : "text-red-500",
            )}
          >
            {m.delta} vs prev window
          </p>
        </div>
      ))}
    </div>
  );
}
