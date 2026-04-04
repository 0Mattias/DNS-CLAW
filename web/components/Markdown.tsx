"use client";

import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import CodeBlock from "./CodeBlock";
import type { ReactNode } from "react";

interface MarkdownProps {
  content: string;
}

export default function Markdown({ content }: MarkdownProps) {
  return (
    <ReactMarkdown
      remarkPlugins={[remarkGfm]}
      components={{
        code({ className, children, ...props }) {
          const match = /language-(\w+)/.exec(className || "");
          const codeStr = String(children).replace(/\n$/, "");

          // Fenced code block (has language class)
          if (match) {
            return <CodeBlock code={codeStr} language={match[1]} />;
          }

          // Check if it's a block code (multi-line or inside pre)
          if (codeStr.includes("\n")) {
            return <CodeBlock code={codeStr} />;
          }

          // Inline code
          return (
            <code
              className="px-1.5 py-0.5 rounded bg-zinc-800 text-[rgb(255,140,110)] text-sm font-mono"
              {...props}
            >
              {children}
            </code>
          );
        },

        // Headings
        h1: ({ children }: { children?: ReactNode }) => (
          <h1 className="text-xl font-bold text-zinc-100 mt-6 mb-3 border-b border-zinc-800 pb-2">
            {children}
          </h1>
        ),
        h2: ({ children }: { children?: ReactNode }) => (
          <h2 className="text-lg font-bold text-zinc-100 mt-5 mb-2">
            {children}
          </h2>
        ),
        h3: ({ children }: { children?: ReactNode }) => (
          <h3 className="text-base font-semibold text-zinc-200 mt-4 mb-2">
            {children}
          </h3>
        ),

        // Paragraphs
        p: ({ children }: { children?: ReactNode }) => (
          <p className="mb-3 leading-relaxed">{children}</p>
        ),

        // Lists
        ul: ({ children }: { children?: ReactNode }) => (
          <ul className="list-disc list-outside ml-5 mb-3 space-y-1">
            {children}
          </ul>
        ),
        ol: ({ children }: { children?: ReactNode }) => (
          <ol className="list-decimal list-outside ml-5 mb-3 space-y-1">
            {children}
          </ol>
        ),

        // Links
        a: ({
          href,
          children,
        }: {
          href?: string;
          children?: ReactNode;
        }) => (
          <a
            href={href}
            target="_blank"
            rel="noopener noreferrer"
            className="text-[rgb(255,140,110)] hover:text-[rgb(255,195,180)] underline underline-offset-2"
          >
            {children}
          </a>
        ),

        // Blockquotes
        blockquote: ({ children }: { children?: ReactNode }) => (
          <blockquote className="border-l-2 border-[rgb(255,60,50)] pl-4 my-3 text-zinc-400 italic">
            {children}
          </blockquote>
        ),

        // Tables
        table: ({ children }: { children?: ReactNode }) => (
          <div className="overflow-x-auto my-3">
            <table className="w-full border-collapse text-sm">
              {children}
            </table>
          </div>
        ),
        th: ({ children }: { children?: ReactNode }) => (
          <th className="border border-zinc-700 px-3 py-1.5 bg-zinc-800/50 text-left font-semibold">
            {children}
          </th>
        ),
        td: ({ children }: { children?: ReactNode }) => (
          <td className="border border-zinc-800 px-3 py-1.5">{children}</td>
        ),

        // Horizontal rule
        hr: () => <hr className="border-zinc-800 my-4" />,

        // Strong / em
        strong: ({ children }: { children?: ReactNode }) => (
          <strong className="font-semibold text-zinc-100">{children}</strong>
        ),
      }}
    >
      {content}
    </ReactMarkdown>
  );
}
