"use client";

import { useState } from "react";

import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

const speeds = ["0.5x", "1x", "2x", "10x"] as const;

export function Header() {
  const [running, setRunning] = useState(true);
  const [speed, setSpeed] = useState<(typeof speeds)[number]>("1x");

  return (
    <header className="flex h-14 items-center justify-between border-b border-border bg-card px-6">
      <div className="flex items-center gap-3">
        <div className="flex size-7 items-center justify-center rounded-md bg-primary text-sm font-bold text-primary-foreground">
          M
        </div>
        <div>
          <p className="text-sm leading-none font-semibold">MarketSim</p>
          <p className="mt-1 text-xs text-muted-foreground">
            Co-Adaptive Exchange Simulation
          </p>
        </div>
        <span
          className={cn(
            "ml-4 inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-xs",
            running
              ? "border-emerald-500/30 bg-emerald-500/10 text-emerald-500"
              : "border-amber-500/30 bg-amber-500/10 text-amber-500",
          )}
        >
          <span
            className={cn(
              "size-1.5 rounded-full",
              running ? "animate-pulse bg-emerald-500" : "bg-amber-500",
            )}
          />
          {running ? `Running · ${speed}` : "Paused"}
        </span>
      </div>
      <div className="flex items-center gap-2">
        <div className="flex overflow-hidden rounded-md border border-border">
          {speeds.map((s) => (
            <button
              key={s}
              onClick={() => setSpeed(s)}
              className={cn(
                "px-2.5 py-1.5 text-xs transition-colors",
                speed === s
                  ? "bg-muted font-medium text-foreground"
                  : "text-muted-foreground hover:text-foreground",
              )}
            >
              {s}
            </button>
          ))}
        </div>
        <Button variant="outline" size="sm" onClick={() => setRunning(!running)}>
          {running ? "Pause" : "Resume"}
        </Button>
        <Button size="sm">Reset</Button>
      </div>
    </header>
  );
}