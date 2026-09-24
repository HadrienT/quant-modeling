import type { MarkInput } from "@/shared/api";

type Status = MarkInput["status"];

/** Worst first: a mark is only as good as its weakest input. */
const ORDER: Status[] = ["default", "proxied", "stale", "observed"];
export const STATUS_TONE = {
	observed: "good",
	stale: "warning",
	proxied: "warning",
	default: "serious",
} as const;
export const MEANING: Record<Status, string> = {
	observed: "read from the database for that date",
	stale: "the last value available is older than it should be",
	proxied: "no market quote for it; an estimate stands in (see methodology)",
	default: "no data at all; the value typed with the trade is kept",
};

export function worstStatus(inputs: MarkInput[]): Status {
	return ORDER.find((s) => inputs.some((i) => i.status === s)) ?? "observed";
}
