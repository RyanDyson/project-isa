import { SidebarTrigger,} from "@/components/ui/sidebar"
import { AppSidebar } from "@/components/global/app-sidebar"

export default function Layout({ children }: { children: React.ReactNode }) {
    return (
        <div className="flex">
            <AppSidebar />
            <main className="flex-1 p-6">
                <SidebarTrigger className="mb-4" />
                {children}
            </main>
        </div>
    )
}