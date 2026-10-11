"use client";

import { createContext, useContext, useState } from "react";
import {
  RiCandleLine,
  RiDashboard2Line,
  RiAddLine,
  RiRobot2Line,
  RiListUnordered,
  RiScales3Line,
  RiStockLine,
} from "@remixicon/react";

import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";

export type WorkspaceDialog = "add-block" | "agents" | "planner" | null;

type WorkspaceContextValue = {
  active: WorkspaceDialog;
  openDialog: (dialog: Exclude<WorkspaceDialog, null>) => void;
  closeDialog: () => void;
};

const WorkspaceContext = createContext<WorkspaceContextValue | null>(null);

export function useWorkspace() {
  const context = useContext(WorkspaceContext);
  if (!context) {
    throw new Error("useWorkspace must be used within a WorkspaceProvider.");
  }

  return context;
}

/**
 * The dashboard is a single drag-and-drop canvas; the sidebar (and later the
 * canvas itself) opens shared dialogs to add blocks or manage agents and the
 * RL planner. The dialogs live here so any component under this provider can
 * trigger them.
 */
export function WorkspaceProvider({ children }: { children: React.ReactNode }) {
  const [active, setActive] = useState<WorkspaceDialog>(null);
  const value: WorkspaceContextValue = {
    active,
    openDialog: (dialog) => setActive(dialog),
    closeDialog: () => setActive(null),
  };

  return (
    <WorkspaceContext.Provider value={value}>
      {children}
      <AddBlockDialog />
      <AgentsDialog />
      <PlannerDialog />
    </WorkspaceContext.Provider>
  );
}

type BlockTypeId = "order-book" | "price-chart" | "trade-tape" | "metrics";

const BLOCK_TYPES: { id: BlockTypeId; title: string; description: string; icon: React.ComponentType<{ size?: number | string }> }[] = [
  {
    id: "order-book",
    title: "Order Book",
    description: "Live depth ladder for a ticker",
    icon: RiListUnordered,
  },
  {
    id: "price-chart",
    title: "Price Chart",
    description: "Price series for a chosen ticker",
    icon: RiStockLine,
  },
  {
    id: "trade-tape",
    title: "Trade Tape",
    description: "Recent fills across the book",
    icon: RiCandleLine,
  },
  {
    id: "metrics",
    title: "Metrics",
    description: "Spread, volatility, liquidity depth",
    icon: RiDashboard2Line,
  },
];

function AddBlockDialog() {
  const { active, closeDialog } = useWorkspace();

  function addBlock(blockId: BlockTypeId) {
    // TODO: insert the block into the drag-and-drop canvas layout
    console.log("[workspace] add block:", blockId);
    closeDialog();
  }

  return (
    <Dialog open={active === "add-block"} onOpenChange={(open) => !open && closeDialog()}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>Add block</DialogTitle>
          <DialogDescription>
            Blocks appear on your workspace canvas. Drag and drop to rearrange
            them.
          </DialogDescription>
        </DialogHeader>

        <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
          {BLOCK_TYPES.map((block) => (
            <Button
              key={block.id}
              variant="outline"
              className="h-auto flex-col items-start gap-1 p-3 text-left"
              onClick={() => addBlock(block.id)}
            >
              <span className="flex items-center gap-2 text-sm font-medium">
                <block.icon />
                {block.title}
              </span>
              <span className="text-muted-foreground text-xs font-normal">
                {block.description}
              </span>
            </Button>
          ))}
        </div>

        <DialogFooter>
          <Button variant="ghost" onClick={closeDialog}>
            Cancel
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

type AgentKind = "market-making" | "momentum" | "spoofing" | "rl";

const AGENT_KINDS: { kind: AgentKind; title: string; running: number }[] = [
  { kind: "market-making", title: "Market-making agents", running: 2 },
  { kind: "momentum", title: "Momentum agents", running: 3 },
  { kind: "spoofing", title: "Spoofing-like agents", running: 1 },
  { kind: "rl", title: "RL trading agents", running: 2 },
];

function AgentsDialog() {
  const { active, closeDialog } = useWorkspace();

  return (
    <Dialog open={active === "agents"} onOpenChange={(open) => !open && closeDialog()}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>Agent pool</DialogTitle>
          <DialogDescription>
            Tune the population of trading agents submitting orders to the
            matching engine.
          </DialogDescription>
        </DialogHeader>

        <div className="flex flex-col gap-2">
          {AGENT_KINDS.map((agent) => (
            <div
              key={agent.kind}
              className="border-border flex items-center justify-between rounded-md border p-3"
            >
              <span className="flex items-center gap-2 text-sm font-medium">
                <RiRobot2Line />
                {agent.title}
              </span>
              <span className="text-muted-foreground text-xs">
                {agent.running} running
              </span>
            </div>
          ))}
          <p className="text-muted-foreground text-xs">
            Configuration UI connects to the backend agent endpoints once
            available.
          </p>
        </div>

        <DialogFooter>
          <Button variant="outline">
            <RiAddLine />
            Add agent
          </Button>
          <Button variant="ghost" onClick={closeDialog}>
            Close
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

function PlannerDialog() {
  const { active, closeDialog } = useWorkspace();

  return (
    <Dialog open={active === "planner"} onOpenChange={(open) => !open && closeDialog()}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>RL exchange planner</DialogTitle>
          <DialogDescription>
            Configure the upper-level policy that reacts to the agent
            population&apos;s behaviour.
          </DialogDescription>
        </DialogHeader>

        <div className="flex flex-col gap-2">
          <div className="border-border flex items-center justify-between rounded-md border p-3">
            <span className="flex items-center gap-2 text-sm font-medium">
              <RiScales3Line />
              Policy mode
            </span>
            <span className="text-muted-foreground text-xs">
              Dynamic (learning) · checkpoint untrained
            </span>
          </div>
          <p className="text-muted-foreground text-xs">
            Controls attach to the planner endpoints once training runs exist.
          </p>
        </div>

        <DialogFooter>
          <Button variant="ghost" onClick={closeDialog}>
            Close
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
