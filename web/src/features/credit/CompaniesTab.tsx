import { useCreditCompanies, useFundamentals } from "@/shared/api";
import { Combobox, Methodology, Segmented } from "@/shared/ui";
import { ErrorState, TableSkeleton } from "@/shared/ui/states";
import { FilingsList } from "./FilingsList";
import { StatementsTable } from "./StatementsTable";
import { StructuralPanel } from "./StructuralPanel";

type Frequency = "annual" | "quarterly";

/**
 * One company: its statements from 10-K / 10-Q filings (every figure linked
 * to its filing), its structural credit model, and its recent filings.
 */
export function CompaniesTab({
	ticker,
	frequency,
	onTicker,
	onFrequency,
}: {
	ticker: string;
	frequency: Frequency;
	onTicker: (t: string) => void;
	onFrequency: (f: Frequency) => void;
}) {
	const companies = useCreditCompanies();
	const fundamentals = useFundamentals(ticker, frequency);
	const data = fundamentals.data;
	const options =
		companies.data?.companies.flatMap((c) =>
			c.tickers.map((t) => ({ value: t, label: `${t} — ${c.name ?? ""}` })),
		) ?? [];

	return (
		<div className="flex flex-col gap-6">
			<div className="flex flex-wrap items-center justify-between gap-3">
				<div className="w-80">
					<Combobox
						options={options}
						value={ticker}
						onChange={onTicker}
						placeholder="Select a company"
					/>
				</div>
				<Segmented
					label="Frequency"
					options={[
						{ value: "annual" as const, label: "Annual (10-K)" },
						{ value: "quarterly" as const, label: "Quarterly (10-Q)" },
					]}
					value={frequency}
					onChange={onFrequency}
				/>
			</div>
			{fundamentals.error ? (
				<ErrorState
					error={fundamentals.error}
					onRetry={() => fundamentals.refetch()}
				/>
			) : !data ? (
				<TableSkeleton rows={12} />
			) : (
				<>
					<div className="flex flex-col gap-0.5">
						<h2 className="text-base font-semibold text-ink">
							{data.company.name}{" "}
							<span className="font-normal text-ink-muted">
								({data.company.tickers.join(", ")})
							</span>
						</h2>
						<p className="text-xs text-ink-muted">
							SIC {data.company.sic} · {data.company.sic_description} · CIK{" "}
							{data.company.cik} · last filing {data.company.last_filed}
						</p>
					</div>
					<StatementsTable data={data} />
					<StructuralPanel ticker={ticker} />
					<FilingsList filings={data.filings} />
					<Methodology
						id="fundamentals-methodology"
						sections={data.methodology}
					/>
				</>
			)}
		</div>
	);
}
