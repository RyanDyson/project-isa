"use client";

import { useState } from "react";
import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import {
  RiLogoutCircleRLine,
  RiRefreshLine,
  RiRobot2Line,
  RiScales3Line,
} from "@remixicon/react";

import {
  Sidebar,
  SidebarContent,
  SidebarFooter,
  SidebarGroup,
  SidebarGroupContent,
  SidebarGroupLabel,
  SidebarHeader,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
  SidebarSeparator,
} from "@/components/ui/sidebar";
import { Button } from "@/components/ui/button";
import { useWorkspace } from "@/components/global/workspace";

export function AppSidebar() {
  const pathname = usePathname();
  const { openDialog } = useWorkspace();

  return (
    <Sidebar variant="inset">
      <SidebarHeader>
        <SidebarMenu>
          <SidebarMenuItem>
            <SidebarMenuButton
              size="lg"
              render={<div className="flex w-full flex-col items-start" />}
            >
              <div className="flex items-center gap-2">
                <div className="bg-primary text-primary-foreground flex size-7 items-center justify-center rounded-md text-sm font-bold">
                  M
                </div>
                <span className="text-sm font-semibold">MarketSim</span>
              </div>
              <span className="text-muted-foreground text-xs">
                Co-Adaptive Exchange Simulation
              </span>
            </SidebarMenuButton>
          </SidebarMenuItem>
        </SidebarMenu>
        <SidebarSeparator />
      </SidebarHeader>

      <SidebarContent>
        <SidebarGroup>
          <SidebarGroupLabel>Workspace</SidebarGroupLabel>
          <SidebarGroupContent>
            <SidebarMenu>
              <SidebarMenuItem>
                <SidebarMenuButton
                  isActive={pathname === "/dashboard"}
                  tooltip="Overview"
                  render={<Link href="/dashboard" />}
                >
                  Overview
                </SidebarMenuButton>
              </SidebarMenuItem>
              <SidebarMenuItem>
                <SidebarMenuButton
                  tooltip="Add a block to the canvas"
                  onClick={() => openDialog("add-block")}
                >
                  Add block
                </SidebarMenuButton>
              </SidebarMenuItem>
            </SidebarMenu>
          </SidebarGroupContent>
        </SidebarGroup>

        <SidebarGroup>
          <SidebarGroupLabel>Simulation controls</SidebarGroupLabel>
          <SidebarGroupContent>
            <SidebarMenu>
              <SidebarMenuItem>
                <SidebarMenuButton
                  tooltip="Running agents"
                  onClick={() => openDialog("agents")}
                >
                  <RiRobot2Line />
                  Agents
                </SidebarMenuButton>
              </SidebarMenuItem>
              <SidebarMenuItem>
                <SidebarMenuButton
                  tooltip="RL exchange planner"
                  onClick={() => openDialog("planner")}
                >
                  <RiScales3Line />
                  RL Planner
                </SidebarMenuButton>
              </SidebarMenuItem>
            </SidebarMenu>
          </SidebarGroupContent>
        </SidebarGroup>

        <SidebarGroup>
          <SidebarGroupLabel>Engine</SidebarGroupLabel>
          <SidebarGroupContent>
            <EngineStatusWidget />
          </SidebarGroupContent>
        </SidebarGroup>
      </SidebarContent>

      <SidebarFooter>
        <SignOutButton />
      </SidebarFooter>
    </Sidebar>
  );
}

/** Placeholder engine playback controls, wired to nothing until the
 * FastAPI control endpoints exist. Lives in the sidebar so the dashboard
 * canvas stays focused on data blocks. */
function EngineStatusWidget() {
  const [running, setRunning] = useState(false);

  return (
    <SidebarMenu>
      <SidebarMenuItem>
        <div className="flex flex-col gap-2 rounded-md border p-2">
          <div className="flex items-center gap-2">
            <span
              className={
                running
                  ? "size-1.5 animate-pulse rounded-full bg-emerald-500"
                  : "size-1.5 rounded-full bg-amber-500"
              }
            />
            <span className="text-xs font-medium">
              {running ? "Running" : "Idle"}
            </span>
          </div>
          <div className="flex gap-1">
            <Button
              size="xs"
              variant={running ? "outline" : "secondary"}
              className="flex-1"
              onClick={() => setRunning((r) => !r)}
            >
              {running ? "Pause" : "Start"}
            </Button>
            <Button size="xs" variant="outline" className="flex-1">
              <RiRefreshLine />
              Reset
            </Button>
          </div>
        </div>
      </SidebarMenuItem>
    </SidebarMenu>
  );
}

function SignOutButton() {
  const router = useRouter();

  async function signOut() {
    await fetch("/api/auth/sign-out", { method: "POST" });
    router.push("/");
    router.refresh();
  }

  return (
    <SidebarMenu>
      <SidebarMenuItem>
        <Button
          variant="ghost"
          size="sm"
          className="w-full justify-start text-muted-foreground"
          onClick={() => void signOut()}
        >
          <RiLogoutCircleRLine />
          Sign out
        </Button>
      </SidebarMenuItem>
    </SidebarMenu>
  );
}
