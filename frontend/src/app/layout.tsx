import "@/styles/globals.css";

import { type Metadata } from "next";
import { Geist, DM_Sans } from "next/font/google";

import { TRPCReactProvider } from "@/trpc/react";
import { cn } from "@/lib/utils";

const dmSans = DM_Sans({subsets:['latin'],variable:'--font-sans'});

export const metadata: Metadata = {
  title: "MarketSim · Co-Adaptive Exchange Simulation",
  description: "Dashboard for the co-adaptive market simulation platform",
  icons: [{ rel: "icon", url: "/favicon.ico" }],
};

const geist = Geist({
  subsets: ["latin"],
  variable: "--font-geist-sans",
});

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en" className={cn("dark", geist.variable, "font-sans", dmSans.variable)}>
      <body>
        <TRPCReactProvider>{children}</TRPCReactProvider>
      </body>
    </html>
  );
}
