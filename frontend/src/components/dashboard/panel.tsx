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
        "border-border bg-card flex flex-col rounded-lg border",
        className,
      )}
    >
      <div className="border-border flex items-center justify-between border-b px-4 py-3">
        <h2 className="text-muted-foreground text-xs font-semibold tracking-wider uppercase">
          {title}
        </h2>
        {action}
      </div>
      <div className="flex-1 p-4">{children}</div>
    </section>
  );
}
