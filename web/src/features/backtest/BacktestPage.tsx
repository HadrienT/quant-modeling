import { useEffect, useMemo, useRef, useState } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { Play, X } from "lucide-react";
import {
	useRunBacktest,
	useTickers,
	type BacktestResponse,
} from "@/shared/api";
import { Badge, Button, Combobox, Field, Label } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { Results } from "./BacktestResults";

/** Backtest (WP 11). Files kept under ~200 lines by splitting form / run / results. */

type Config = {
	tickers: string[];
	opt_start: string;
	opt_end: string;
	initial_capital: number;
	max_share: number;
	min_share: number;
	rebalance_freq: number;
};

const DEFAULT: Config = {
	tickers: ["AAPL", "MSFT", "XOM", "JPM"],
	opt_start: "2015-01-02",
	opt_end: "2020-01-02",
	initial_capital: 100_000,
	max_share: 0.4,
	min_share: 0,
	rebalance_freq: 0,
};

const enc = (c: Config) =>
	btoa(unescape(encodeURIComponent(JSON.stringify(c))))
		.replace(/\+/g, "-")
		.replace(/\//g, "_")
		.replace(/=+$/, "");
const dec = (s?: string): Config => {
	if (!s) return DEFAULT;
	try {
		return {
			...DEFAULT,
			...JSON.parse(
				decodeURIComponent(
					escape(atob(s.replace(/-/g, "+").replace(/_/g, "/"))),
				),
			),
		};
	} catch {
		return DEFAULT;
	}
};

export default function BacktestPage() {
	const search = useSearch({ from: "/backtest" });
	const navigate = useNavigate({ from: "/backtest" });
	const cfg = useMemo(() => dec(search.c), [search.c]);
	const tickers = useTickers();
	const run = useRunBacktest();

	const [elapsed, setElapsed] = useState(0);
	const timer = useRef<ReturnType<typeof setInterval>>(null);
	const [runs, setRuns] = useState<{ label: string; data: BacktestResponse }[]>(
		[],
	);

	useEffect(() => {
		if (run.isPending) {
			setElapsed(0);
			timer.current = setInterval(() => setElapsed((e) => e + 1), 1000);
		} else if (timer.current) {
			clearInterval(timer.current);
		}
		return () => {
			if (timer.current) clearInterval(timer.current);
		};
	}, [run.isPending]);

	const setCfg = (patch: Partial<Config>) =>
		navigate({ search: () => ({ c: enc({ ...cfg, ...patch }) }) });

	const errors: string[] = [];
	if (cfg.opt_start >= cfg.opt_end)
		errors.push("opt_start must be before opt_end");
	if (cfg.min_share > cfg.max_share)
		errors.push("min_share must be ≤ max_share");
	if (cfg.tickers.length < 2) errors.push("pick at least two tickers");

	const result = run.data;

	return (
		<div className="mx-auto flex max-w-5xl flex-col gap-5">
			<h1 className="text-lg font-semibold text-ink">Backtest</h1>

			<div className="grid gap-3 rounded-md border border-hairline bg-surface p-4 sm:grid-cols-2">
				<div className="sm:col-span-2">
					<Label>Tickers</Label>
					<div className="mt-1 flex flex-wrap gap-1.5">
						{cfg.tickers.map((t) => (
							<Badge key={t} tone="accent">
								{t}
								<button
									type="button"
									aria-label={`Remove ${t}`}
									className="ml-1"
									onClick={() =>
										setCfg({ tickers: cfg.tickers.filter((x) => x !== t) })
									}
								>
									<X className="size-3" />
								</button>
							</Badge>
						))}
						<div className="w-40">
							<Combobox
								options={(tickers.data?.tickers ?? [])
									.filter((t) => !cfg.tickers.includes(t))
									.map((t) => ({ value: t }))}
								value={null}
								onChange={(t) => setCfg({ tickers: [...cfg.tickers, t] })}
								placeholder="Add…"
							/>
						</div>
					</div>
				</div>

				<Field
					label="Optimisation start"
					type="date"
					value={cfg.opt_start}
					onChange={(e) => setCfg({ opt_start: e.target.value })}
				/>
				<Field
					label="Investment start (opt end)"
					type="date"
					value={cfg.opt_end}
					onChange={(e) => setCfg({ opt_end: e.target.value })}
				/>
				<Field
					label="Initial capital"
					type="number"
					value={cfg.initial_capital}
					onChange={(e) => setCfg({ initial_capital: Number(e.target.value) })}
				/>
				<Field
					label="Rebalance every N days (0 = static)"
					type="number"
					value={cfg.rebalance_freq}
					onChange={(e) => setCfg({ rebalance_freq: Number(e.target.value) })}
				/>
				<Field
					label="Max weight"
					type="number"
					step={0.05}
					value={cfg.max_share}
					onChange={(e) => setCfg({ max_share: Number(e.target.value) })}
				/>
				<Field
					label="Min weight (drop below)"
					type="number"
					step={0.01}
					value={cfg.min_share}
					onChange={(e) => setCfg({ min_share: Number(e.target.value) })}
				/>
			</div>

			{errors.length > 0 && (
				<ul className="text-xs text-critical">
					{errors.map((e) => (
						<li key={e}>• {e}</li>
					))}
				</ul>
			)}

			<div className="flex items-center gap-3">
				<Button
					disabled={errors.length > 0 || run.isPending}
					onClick={() => run.mutate(cfg)}
				>
					<Play className="size-3.5" />
					{run.isPending ? `Running… ${elapsed}s` : "Run backtest"}
				</Button>
				{result && (
					<Button
						size="sm"
						variant="ghost"
						onClick={() =>
							setRuns((r) => [
								...r,
								{ label: `run ${r.length + 1}`, data: result },
							])
						}
					>
						Pin this run
					</Button>
				)}
			</div>

			{run.error && (
				<ErrorState error={run.error} onRetry={() => run.mutate(cfg)} />
			)}

			{result && <Results result={result} optWindow={cfg} pinned={runs} />}
		</div>
	);
}
