import localFont from "next/font/local";
import { cn } from "@/lib/utils"

const appleGaramond = localFont({
  src: "@frontend/public/fonts/Apple Garamond Book.woff2",
  weight: "400",
  display: "swap",
  variable: "--font-apple-garamond",
});

export function DisplayText({children, className, ...props}: {children: React.ReactNode, className: string} & React.HTMLAttributes<HTMLSpanElement>) {
    return (<span className={cn(appleGaramond.className, className)} {...props}>{children}</span>)
}