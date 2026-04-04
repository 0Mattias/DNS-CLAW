import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "DNS-CLAW",
  description: "AI chat tunneled through DNS",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en" className="dark">
      <body className="antialiased">{children}</body>
    </html>
  );
}
