import { useEffect, useMemo, useRef, useState } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { Play, X } from "lucide-react";
import {
	useRunBacktest,
	useTickers,
	type BacktestResponse,
} from "@/shared/api";
import {
	Badge,
	Button,
	Combobox,
	Input,
	Label,
	Metric,
	MetricRow,
} from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { AllocationChart, EquityAndDrawdownChart } from "@/shared/viz";

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
					onChange={(v) => setCfg({ opt_start: v })}
				/>
				<Field
					label="Investment start (opt end)"
					type="date"
					value={cfg.opt_end}
					onChange={(v) => setCfg({ opt_end: v })}
				/>
				<Field
					label="Initial capital"
					type="number"
					value={cfg.initial_capital}
					onChange={(v) => setCfg({ initial_capital: Number(v) })}
				/>
				<Field
					label="Rebalance every N days (0 = static)"
					type="number"
					value={cfg.rebalance_freq}
					onChange={(v) => setCfg({ rebalance_freq: Number(v) })}
				/>
				<Field
					label="Max weight"
					type="number"
					step={0.05}
					value={cfg.max_share}
					onChange={(v) => setCfg({ max_share: Number(v) })}
				/>
				<Field
					label="Min weight (drop below)"
					type="number"
					step={0.01}
					value={cfg.min_share}
					onChange={(v) => setCfg({ min_share: Number(v) })}
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

function Results({
	result,
	optWindow,
	pinned,
}: {
	result: BacktestResponse;
	optWindow: { opt_start: string; opt_end: string };
	pinned: { label: string; data: BacktestResponse }[];
}) {
	const m = result.metrics;
	const fmt = (v: number | null | undefined, suffix = "") =>
		v == null ? "n/d" : `${v.toFixed(2)}${suffix}`;

	return (
		<div className="flex flex-col gap-4">
			{result.warnings.length > 0 && (
				<div className="rounded-md border border-warning/40 bg-warning/5 p-3">
					<p className="text-xs font-medium text-warning">Warnings</p>
					<ul className="mt-1 list-disc pl-4 text-xs text-ink-secondary">
						{result.warnings.map((w, i) => (
							<li key={i}>{w}</li>
						))}
					</ul>
				</div>
			)}

			<p className="text-2xs text-ink-muted">
				The grey band is the in-sample optimisation window (
				{optWindow.opt_start} → {optWindow.opt_end}). Not modelled: transaction
				costs, slippage, survivorship bias, dividend treatment. Sharpe uses the
				API's risk-free assumption and √252 annualisation.
			</p>

			<EquityAndDrawdownChart
				portfolio={result.portfolio_values.map((p) => ({
					time: p.date,
					value: p.value,
				}))}
				benchmark={result.sp500_values?.map((p) => ({
					time: p.date,
					value: p.value,
				}))}
				optWindow={{ start: optWindow.opt_start, end: optWindow.opt_end }}
			/>

			<MetricRow>
				<Metric label="Total return" value={fmt(m.total_return_pct, "%")} />
				<Metric label="Annualised" value={fmt(m.annualized_return_pct, "%")} />
				<Metric label="Max drawdown" value={fmt(m.max_drawdown * 100, "%")} />
				<Metric
					label="Sharpe (realised)"
					value={fmt(m.sharpe_ratio)}
					footnote={`in-sample optimum ${fmt(m.optimal_sharpe)}`}
				/>
				<Metric
					label="Alpha / Beta"
					value={`${fmt(m.alpha)} / ${fmt(m.beta)}`}
					footnote={m.alpha == null ? "not computable" : undefined}
				/>
			</MetricRow>

			<AllocationChart
				title="Optimised allocation"
				rows={result.allocation.map((a) => ({
					label: a.ticker,
					weight: a.weight,
					returnPct: a.return_pct,
				}))}
			/>

			{pinned.length > 0 && (
				<div className="text-2xs text-ink-muted">
					{pinned.length} run(s) pinned — overlay comparison shows parameter
					sensitivity.
				</div>
			)}
		</div>
	);
}

function Field({
	label,
	type,
	value,
	step,
	onChange,
}: {
	label: string;
	type: string;
	value: string | number;
	step?: number;
	onChange: (v: string) => void;
}) {
	return (
		<label className="flex flex-col gap-1">
			<Label>{label}</Label>
			<Input
				type={type}
				step={step}
				value={value}
				onChange={(e) => onChange(e.target.value)}
			/>
		</label>
	);
}
