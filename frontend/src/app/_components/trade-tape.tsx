import { Panel } from "@/app/_components/panel";
import { cn } from "@/lib/utils";

type Trade = {
  time: string;
  price: number;
  size: number;
  side: "buy" | "sell";
};

const TRADES: Trade[] = [
  { time: "14:32:07.412", price: 101.42, size: 100, side: "buy" },
  { time: "14:32:07.398", price: 101.41, size: 240, side: "sell" },
  { time: "14:32:07.355", price: 101.44, size: 80, side: "buy" },
  { time: "14:32:07.301", price: 101.43, size: 320, side: "buy" },
  { time: "14:32:07.255", price: 101.4, size: 120, side: "sell" },
  { time: "14:32:07.190", price: 101.45, size: 60, side: "buy" },
  { time: "14:32:07.133", price: 101.43, size: 210, side: "sell" },
  { time: "14:32:07.081", price: 101.42, size: 450, side: "buy" },
  { time: "14:32:07.012", price: 101.39, size: 90, side: "sell" },
  { time: "14:32:06.987", price: 101.41, size: 140, side: "buy" },
];

export function TradeTape() {
  return (
    <Panel
      title="Trade Tape"
      action={<span className="text-xs text-muted-foreground">last 10</span>}
    >
      <div className="grid grid-cols-3 gap-2 text-xs text-muted-foreground">
        <span>Time</span>
        <span className="text-right">Price</span>
        <span className="text-right">Size</span>
      </div>
      <div className="mt-1 space-y-0.5">
        {TRADES.map((t) => (
          <div
            key={t.time}
            className="grid grid-cols-3 gap-2 rounded px-1 py-0.5 text-xs tabular-nums hover:bg-muted/50"
          >
            <span className="text-muted-foreground">{t.time}</span>
            <span
              className={cn(
                "text-right",
                t.side === "buy" ? "text-emerald-500" : "text-red-500",
              )}
            >
              {t.price.toFixed(2)}
            </span>
            <span className="text-right text-muted-foreground">{t.size}</span>
          </div>
        ))}
      </div>
    </Panel>
  );
}
