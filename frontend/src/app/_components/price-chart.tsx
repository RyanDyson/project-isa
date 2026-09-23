import { Panel } from "@/app/_components/panel";

const POINTS = 72;

function series() {
  return Array.from({ length: POINTS }, (_, i) => {
    const drift = 100 + i * 0.022;
    const wave = Math.sin(i / 7) * 1.15 + Math.sin(i / 13) * 0.85;
    return drift + wave;
  });
}

export function PriceChart() {
  const data = series();
  const min = Math.min(...data);
  const max = Math.max(...data);
  const w = 600;
  const h = 200;
  const pad = 8;

  const points = data.map((v, i) => {
    const x = (i / (POINTS - 1)) * w;
    const y = pad + (1 - (v - min) / (max - min)) * (h - 2 * pad);
    return [x, y] as const;
  });
  const line = points.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(" ");
  const area = `0,${h} ${line} ${w},${h}`;
  const last = data[data.length - 1] ?? 0;

  return (
    <Panel
      title="Price / Time"
      action={
        <span className="rounded-full border border-emerald-500/30 bg-emerald-500/10 px-2 py-0.5 text-xs text-emerald-500 tabular-nums">
          {last.toFixed(2)} · +0.84%
        </span>
      }
      className="lg:col-span-2"
    >
      <div className="relative h-56">
        <svg
          viewBox={`0 0 ${w} ${h}`}
          preserveAspectRatio="none"
          className="h-full w-full"
        >
          <defs>
            <linearGradient id="priceFill" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0%" stopColor="oklch(0.7 0.15 160)" stopOpacity="0.25" />
              <stop offset="100%" stopColor="oklch(0.7 0.15 160)" stopOpacity="0" />
            </linearGradient>
          </defs>
          {[0.25, 0.5, 0.75].map((f) => (
            <line
              key={f}
              x1="0"
              x2={w}
              y1={pad + f * (h - 2 * pad)}
              y2={pad + f * (h - 2 * pad)}
              stroke="oklch(1 0 0 / 8%)"
              strokeDasharray="4 4"
            />
          ))}
          <polygon points={area} fill="url(#priceFill)" />
          <polyline
            points={line}
            fill="none"
            stroke="oklch(0.72 0.17 160)"
            strokeWidth="2"
            vectorEffect="non-scaling-stroke"
          />
        </svg>
        <span className="absolute top-0 left-0 text-xs text-muted-foreground tabular-nums">
          {max.toFixed(2)}
        </span>
        <span className="absolute bottom-0 left-0 text-xs text-muted-foreground tabular-nums">
          {min.toFixed(2)}
        </span>
      </div>
      <div className="mt-2 flex justify-between text-xs text-muted-foreground">
        <span>T-60s</span>
        <span>T-30s</span>
        <span>now</span>
      </div>
    </Panel>
  );
}
