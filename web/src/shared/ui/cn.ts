import { type ClassValue, clsx } from "clsx";
import { twMerge } from "tailwind-merge";

/** shadcn convention: merge conditional class names, dedupe Tailwind conflicts. */
export function cn(...inputs: ClassValue[]): string {
	return twMerge(clsx(inputs));
}
