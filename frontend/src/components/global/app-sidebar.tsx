"use client";

import { useState } from "react";
import { useRouter } from "next/navigation";
import {
  RiLogoutCircleRLine,
  RiRefreshLine,
  RiRobot2Line,
  RiScales3Line,
  RiAddLine,
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

} from "@/components/ui/sidebar";
import { Button } from "@/components/ui/button";

export function AppSidebar() {

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
                <span className="text-sm font-semibold">Agentic Trading Sandbox</span>
              </div>
            </SidebarMenuButton>
          </SidebarMenuItem>
        </SidebarMenu>
      </SidebarHeader>

      <SidebarContent>
        <SidebarGroup>
        <SidebarGroupContent>
            <SidebarMenu>
              <SidebarMenuItem>
                <Button variant="gradient_primary" className="w-full">
                    Add View
                    <RiAddLine className="size-4" />
                </Button>
              </SidebarMenuItem>
            </SidebarMenu>
          </SidebarGroupContent>
          <SidebarGroupLabel>Workspace</SidebarGroupLabel>

        </SidebarGroup>

        <SidebarGroup>
          <SidebarGroupLabel>Simulation controls</SidebarGroupLabel>
          <SidebarGroupContent>
            <SidebarMenu>
            
            </SidebarMenu>
          </SidebarGroupContent>
        </SidebarGroup>

       
      </SidebarContent>

      <SidebarFooter>
        <SignOutButton />
      </SidebarFooter>
    </Sidebar>
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
