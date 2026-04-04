"use client";

import { useRef, useEffect, type KeyboardEvent, type ChangeEvent } from "react";

interface InputProps {
  value: string;
  onChange: (value: string) => void;
  onSubmit: () => void;
  disabled?: boolean;
  placeholder?: string;
}

export default function Input({
  value,
  onChange,
  onSubmit,
  disabled,
  placeholder,
}: InputProps) {
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  // Auto-resize textarea
  useEffect(() => {
    const el = textareaRef.current;
    if (!el) return;
    el.style.height = "auto";
    el.style.height = Math.min(el.scrollHeight, 200) + "px";
  }, [value]);

  // Focus on mount
  useEffect(() => {
    textareaRef.current?.focus();
  }, []);

  const handleKeyDown = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      if (value.trim() && !disabled) {
        onSubmit();
      }
    }
  };

  const handleChange = (e: ChangeEvent<HTMLTextAreaElement>) => {
    onChange(e.target.value);
  };

  return (
    <div className="flex items-end gap-3 border-t border-zinc-800 bg-zinc-950 px-4 py-3">
      <div className="flex-1 flex items-end gap-2 rounded-lg border border-zinc-800 bg-zinc-900 px-3 py-2 focus-within:border-[rgb(255,60,50)]/50 transition-colors">
        <span className="text-[rgb(255,100,80)] font-mono text-sm font-bold pb-0.5 select-none">
          {">"}
        </span>
        <textarea
          ref={textareaRef}
          value={value}
          onChange={handleChange}
          onKeyDown={handleKeyDown}
          disabled={disabled}
          placeholder={placeholder || "Type a message..."}
          rows={1}
          className="flex-1 bg-transparent text-zinc-200 text-sm font-mono placeholder:text-zinc-600 outline-none resize-none disabled:opacity-50"
        />
      </div>
      <button
        onClick={onSubmit}
        disabled={disabled || !value.trim()}
        className="px-4 py-2 rounded-lg bg-[rgb(255,60,50)] text-white text-sm font-semibold hover:bg-[rgb(255,100,80)] disabled:opacity-30 disabled:cursor-not-allowed transition-colors"
      >
        Send
      </button>
    </div>
  );
}
