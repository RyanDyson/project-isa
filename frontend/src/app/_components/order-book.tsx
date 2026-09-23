import { Panel } from "@/app/_components/panel";
import { cn } from "@/lib/utils";

type Level = { price: number; size: number };

const asks: Level[] = [
  { price: 101.48, size: 320 },
  { price: 101.46, size: 150 },
  { price: 101.45, size: 610 },
  { price: 101.44, size: 240 },
  { price: 101.43, size: 90 },
  { price: 101.42, size: 410 },
  { price: 101.41, size: 180 },
  { price: 101.4, size: 520 },
];

const bids: Level[] = [
  { price: 101.39, size: 260 },
  { price: 101.38, size: 430 },
  { price: 101.37, size: 120 },
  { price: 101.36, size: 580 },
  { price: 101.35, size: 300 },
  { price: 101.34, size: 210 },
  { price: 101.33, size: 640 },
  { price: 101.32, size: 170 },
];

const maxSize = 660;

function Row({ level, side }: { level: Level; side: "bid" | "ask" }) {
  return (
    <div className="relative flex items-center justify-between px-4 py-1 text-xs tabular-nums">
      <div
        className={cn(
          "absolute inset-y-0 right-0 opacity-15",
          side === "bid" ? "bg-emerald-500" : "bg-red-500",
        )}
        style={{ width: `${(level.size / maxSize) * 100}%` }}
      />
      <span className={side === "bid" ? "text-emerald-500" : "text-red-500"}>
        {level.price.toFixed(2)}
      </span>
      <span className="text-muted-foreground">{level.size}</span>
    </div>
  );
}

export function OrderBook() {
  return (
    <Panel
      title="Order Book · AAPL"
      action={<span className="text-xs text-muted-foreground">Spread 0.042</span>}
    >
      <div className="grid grid-cols-2 text-xs text-muted-foreground">
        <span className="px-4">Price</span>
        <span className="px-4 text-right">Size</span>
      </div>
      <div className="mt-1">
        {[...asks].reverse().map((l) => (
          <Row key={l.price} level={l} side="ask" />
        ))}
      </div>
      <div className="my-1.5 flex items-center justify-between border-y border-border bg-muted/40 px-4 py-1.5 text-xs font-semibold tabular-nums">
        <span>{(101.39 + 101.48).toFixed(2)}</span>
        <span className="font-normal text-muted-foreground">mid</span>
        <span>{(101.48 - 101.39).toFixed(2)}</span>
      </div>
      <div>
        {bids.map((l) => (
          <Row key={l.price} level={l} side="bid" />
        ))}
      </div>
    </Panel>
  );
}
