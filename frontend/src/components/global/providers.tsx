import { SidebarProvider } from "@/components/ui/sidebar";
import { WorkspaceProvider } from "@/components/global/workspace";

export function Providers({ children }: { children: React.ReactNode }) {
  return (
    <WorkspaceProvider>
      <SidebarProvider>{children}</SidebarProvider>
    </WorkspaceProvider>
  );
}
