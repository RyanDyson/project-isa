import type { ReactNode } from "react";

import { cn } from "@/lib/utils";

export function Panel({
  title,
  action,
  className,
  children,
}: {
  title: string;
  action?: ReactNode;
  className?: string;
  children: ReactNode;
}) {
  return (
    <section
      className={cn(
        "flex flex-col rounded-lg border border-border bg-card",
        className,
      )}
    >
      <div className="flex items-center justify-between border-b border-border px-4 py-3">
        <h2 className="text-xs font-semibold tracking-wider text-muted-foreground uppercase">
          {title}
        </h2>
        {action}
      </div>
      <div className="flex-1 p-4">{children}</div>
    </section>
  );
}
