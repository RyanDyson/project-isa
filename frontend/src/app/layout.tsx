import "@/styles/globals.css";

import { type Metadata } from "next";
import localFont from "next/font/local";

import { TRPCReactProvider } from "@/trpc/react";
import { cn } from "@/lib/utils";
import { Providers } from "@/components/global/providers";

const sfPro = localFont({
  src: "../../public/fonts/sf-pro-text_regular.woff2",
  weight: "400",
  display: "swap",
  variable: "--font-sf-pro",
});

export const metadata: Metadata = {
  title: "MarketSim · Co-Adaptive Exchange Simulation",
  description: "Dashboard for the co-adaptive market simulation platform",
  icons: [{ rel: "icon", url: "/favicon.ico" }],
};

export default function RootLayout({
  children,
}: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en" className={cn(sfPro.className, "font-sans")}>
      <body>
        <Providers>{children}</Providers>
      </body>
    </html>
  );
}
